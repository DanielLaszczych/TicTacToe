#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <semaphore.h>
#include <time.h>

#include "server.h"
#include "player_registry.h"
#include "jeux_globals.h"
#include "debug.h"
#include "csapp.h"


int get_int_len (int value) {
  int l = !value;
  while(value){ l++; value/=10; }
  return l;
}

/*
 * Thread function for the thread that handles a particular client.
 *
 * @param  Pointer to a variable that holds the file descriptor for
 * the client connection.  This pointer must be freed once the file
 * descriptor has been retrieved.
 * @return  NULL
 *
 * This function executes a "service loop" that receives packets from
 * the client and dispatches to appropriate functions to carry out
 * the client's requests.  It also maintains information about whether
 * the client has logged in or not.  Until the client has logged in,
 * only LOGIN packets will be honored.  Once a client has logged in,
 * LOGIN packets will no longer be honored, but other packets will be.
 * The service loop ends when the network connection shuts down and
 * EOF is seen.  This could occur either as a result of the client
 * explicitly closing the connection, a timeout in the network causing
 * the connection to be closed, or the main thread of the server shutting
 * down the connection as part of graceful termination.
 */
void *jeux_client_service(void *arg) {
	int connfd = *((int *)arg);
	pthread_detach(pthread_self());
	free(arg);
	debug("%ld: [%d] Starting client service", pthread_self(), connfd);
	CLIENT *client = creg_register(client_registry, connfd);
	if (client == NULL) {
		debug("%ld: [%d] Failed to start client server", pthread_self(), connfd);
		close(connfd);
		return NULL;
	}
	int loggedIn = 0;
	while (1) {
		JEUX_PACKET_HEADER *hdr = malloc(sizeof(JEUX_PACKET_HEADER));
		void **payloadp = malloc(sizeof(void*));
		int recvValue = proto_recv_packet(connfd, hdr, payloadp);
		if (recvValue == -1) {
			close(connfd);
			if (loggedIn) {
				PLAYER *player = client_get_player(client);
				player_unref(player, "because server thread is discarding reference to logged in player");
				debug("%ld: [%d] Logging out client", pthread_self(), connfd);
				client_logout(client);
			}
			creg_unregister(client_registry, client);
			debug("%ld: [%d] Ending client service", pthread_self(), connfd);
			free(hdr);
			if (*payloadp != NULL) {
				free(*payloadp);
			}
			free(payloadp);
			return NULL;
		} else {
			if (hdr->type == JEUX_LOGIN_PKT) {
				debug("%ld: [%d] LOGIN packet received", pthread_self(), connfd);
				if (!loggedIn) {
					debug("%ld: [%d] Login '%s'", pthread_self(), connfd, (char*)*payloadp);
					CLIENT *existingClient = creg_lookup(client_registry, *payloadp);
					if (existingClient != NULL) {
						debug("%ld: [%d] Client %p is already logged in with that username [%s]", pthread_self(), connfd, existingClient, (char*)*payloadp);
						client_unref(existingClient, "becuase the lookup of the client is no longer needed");
						client_send_nack(client);
					} else {
						PLAYER *player = preg_register(player_registry, *payloadp);
						int error = client_login(client, player);
						if (error == -1) {
							client_send_nack(client);
						} else {
							loggedIn = 1;
							client_send_ack(client, NULL, 0);
						}
					}
				} else {
					debug("%ld: [%d] Already logged in (player %p [%s])", pthread_self(), connfd, (void *)client_get_player(client), player_get_name(client_get_player(client)));
					client_send_nack(client);
				}
			} else if (hdr->type == JEUX_USERS_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] USERS packet received", pthread_self(), connfd);
					PLAYER **playerList = creg_all_players(client_registry);
					debug("%ld: [%d] Users", pthread_self(), connfd);
					char *targetBuffer = NULL;
   					int bufferSize = 1;
   					PLAYER **freePlayerList = playerList;
					while (*playerList != NULL) {
						char *tmp;
						int playerRatingLength = get_int_len(player_get_rating(*playerList));
						int totalLength = strlen(player_get_name(*playerList)) + playerRatingLength + 2; // for tab newline
						tmp = realloc(targetBuffer, sizeof *tmp * (bufferSize + totalLength));
						if (tmp) {
							char playerRating[playerRatingLength + 1];
						    snprintf(playerRating, playerRatingLength + 1, "%d", player_get_rating(*playerList));
							targetBuffer = tmp;
							if (bufferSize == 1) {
								strcpy(targetBuffer, player_get_name(*playerList));
							} else {
								strcat(targetBuffer, player_get_name(*playerList));
							}
							bufferSize += totalLength;
							strcat(targetBuffer, "\t");
							strcat(targetBuffer, playerRating);
							strcat(targetBuffer, "\n");
							player_unref(*playerList, "for player removed from players list");
						}
						else {
							free(freePlayerList);
						 	free(targetBuffer);
						 	targetBuffer = NULL;
						 	bufferSize = 0;
						 	debug("%ld: [%d] Unable to allocate or extend input buffer", pthread_self(), connfd);
						 	client_send_nack(client);
						 	free(hdr);
							free(*payloadp);
							free(payloadp);
						 	continue;
						}
						playerList++;
					}
					bufferSize = (bufferSize - 1);
					client_send_ack(client, targetBuffer, bufferSize);
					free(freePlayerList);
					free(targetBuffer);
				}
			} else if (hdr->type == JEUX_INVITE_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] INVITE packet received", pthread_self(), connfd);
					debug("%ld: [%d] Invite '%s'", pthread_self(), connfd, (char *)*payloadp);
					CLIENT *target = creg_lookup(client_registry, *payloadp);
					if (target == NULL) {
						debug("%ld: [%d] No client logged in as user '%s'", pthread_self(), connfd, (char *)*payloadp);
						client_send_nack(client);
					} else {
						GAME_ROLE targetRole;
						GAME_ROLE sourceRole;
						if (hdr->role == 1) {
							targetRole = FIRST_PLAYER_ROLE;
							sourceRole = SECOND_PLAYER_ROLE;
						} else if (hdr->role == 2) {
							targetRole = SECOND_PLAYER_ROLE;
							sourceRole = FIRST_PLAYER_ROLE;
						}
						int error = client_make_invitation(client, target, sourceRole, targetRole);
						client_unref(target, "after invitation attempt");
						if (error == -1) {
							client_send_nack(client);
						} else {
							client_send_ack(client, NULL, 0);
						}
					}
				}
			} else if (hdr->type == JEUX_REVOKE_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] REVOKE packet received", pthread_self(), connfd);
					debug("%ld: [%d] Revoke '%d'", pthread_self(), connfd, hdr->id);
					int error = client_revoke_invitation(client, hdr->id);
					if (error == -1) {
						client_send_nack(client);
					} else {
						client_send_ack(client, NULL, 0);
					}
				}
			} else if (hdr->type == JEUX_DECLINE_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] DECLINE packet received", pthread_self(), connfd);
					debug("%ld: [%d] Decline '%d'", pthread_self(), connfd, hdr->id);
					int error = client_decline_invitation(client, hdr->id);
					if (error == -1) {
						client_send_nack(client);
					} else {
						client_send_ack(client, NULL, 0);
					}
				}
			} else if (hdr->type == JEUX_ACCEPT_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] ACCEPT packet received", pthread_self(), connfd);
					debug("%ld: [%d] Accept '%d'", pthread_self(), connfd, hdr->id);
					char **strp = (char**)malloc(sizeof(char*));
					int error = client_accept_invitation(client, hdr->id, strp);
					if (error == -1) {
						client_send_nack(client);
					} else {
						if (*strp != NULL) {
							int len = strlen(*strp);
							client_send_ack(client, *strp, len);
						} else {
							client_send_ack(client, NULL, 0);
						}
					}
					if (*strp != NULL) {
						free(*strp);
					}
					free(strp);
				}
			} else if (hdr->type == JEUX_MOVE_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] MOVE packet received", pthread_self(), connfd);
					debug("%ld: [%d] Move '%d' (%s)", pthread_self(), connfd, hdr->id, (char*)*payloadp);
					int error = client_make_move(client, hdr->id, (char*)*payloadp);
					if (error == -1) {
						client_send_nack(client);
					} else {
						client_send_ack(client, NULL, 0);
					}
				}
			} else if (hdr->type == JEUX_RESIGN_PKT) {
				if (!loggedIn) {
					debug("%ld: [%d] Login required", pthread_self(), connfd);
					client_send_nack(client);
				} else {
					debug("%ld: [%d] RESIGN packet received", pthread_self(), connfd);
					debug("%ld: [%d] Resign '%d'", pthread_self(), connfd, hdr->id);
					int error = client_resign_game(client, hdr->id);
					if (error == -1) {
						client_send_nack(client);
					} else {
						client_send_ack(client, NULL, 0);
					}
				}
			}
			free(hdr);
			if (*payloadp != NULL) {
				free(*payloadp);
			}
			free(payloadp);
		}
	}
	return NULL;
}
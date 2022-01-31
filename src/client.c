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
#include <time.h>

#include "debug.h"
#include "csapp.h"
#include "server.h"
#include "client.h"


/*
 * A CLIENT represents the state of a network client connected to the
 * system.  It contains the file descriptor of the connection to the
 * client and it provides functions for sending packets to the client.
 * If the client is logged in as a particular player, it contains a
 * reference to a PLAYER object and it contains a list of invitations
 * for which the client is either the source or the target.  CLIENT
 * objects are managed by the client registry.  So that a CLIENT
 * object can be passed around externally to the client registry
 * without fear of dangling references, it has a reference count that
 * corresponds to the number of references that exist to the object.
 * A CLIENT object will not be freed until its reference count reaches zero.
 */

typedef struct invite_node {
	int id;
	INVITATION *invitation;
	struct invite_node *next;
} INVITE_NODE;

/*
 * The CLIENT type is a structure type that defines the state of a client.
 * You will have to give a complete structure definition in client.c.
 * The precise contents are up to you.  Be sure that all the operations
 * that might be called concurrently are thread-safe.
 */
struct client {
	int fd;
	PLAYER *player;
	INVITE_NODE *inviteHead;
	int count;
	int invites;
	pthread_mutex_t clientMutex;
	pthread_mutexattr_t mutexAttr;
};



/*
 * Create a new CLIENT object with a specified file descriptor with which
 * to communicate with the client.  The returned CLIENT has a reference
 * count of one and is in the logged-out state.
 *
 * @param creg  The client registry in which to create the client.
 * @param fd  File descriptor of a socket to be used for communicating
 * with the client.
 * @return  The newly created CLIENT object, if creation is successful,
 * otherwise NULL.
 */
CLIENT *client_create(CLIENT_REGISTRY *creg, int fd) {
	CLIENT *client = malloc(sizeof(CLIENT));
	client->fd = fd;
	client->player = NULL;
	client->inviteHead = NULL;
	client->count = 0;
	client->invites = 0;
	pthread_mutexattr_init(&client->mutexAttr);
	pthread_mutexattr_settype(&client->mutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&client->clientMutex, &client->mutexAttr);
	client_ref(client, "for newly created client");
	return client;
}

/*
 * Increase the reference count on a CLIENT by one.
 *
 * @param client  The CLIENT whose reference count is to be increased.
 * @param why  A string describing the reason why the reference count is
 * being increased.  This is used for debugging printout, to help trace
 * the reference counting.
 * @return  The same CLIENT that was passed as a parameter.
 */
CLIENT *client_ref(CLIENT *client, char *why) {
	pthread_mutex_lock(&client->clientMutex);
	debug("%ld: Increase refrence count on client %p (%d -> %d) %s", pthread_self(), client, client->count, client->count + 1, why);
	client->count = client->count + 1;
	pthread_mutex_unlock(&client->clientMutex);
	return client;
}

/*
 * Decrease the reference count on a CLIENT by one.  If after
 * decrementing, the reference count has reached zero, then the CLIENT
 * and its contents are freed.
 *
 * @param client  The CLIENT whose reference count is to be decreased.
 * @param why  A string describing the reason why the reference count is
 * being decreased.  This is used for debugging printout, to help trace
 * the reference counting.
 */
void client_unref(CLIENT *client, char *why) {
	pthread_mutex_lock(&client->clientMutex);
	debug("%ld: Decrease refrence count on client %p (%d -> %d) %s", pthread_self(), client, client->count, client->count - 1, why);
	client->count = client->count - 1;
	if (client->count == 0) {
		debug("%ld: Free client %p", pthread_self(), client);
		free(client);
		return;
	}
	pthread_mutex_unlock(&client->clientMutex);
}

/*
 * Log in this CLIENT as a specified PLAYER.
 * The login fails if the CLIENT is already logged in or there is already
 * some other CLIENT that is logged in as the specified PLAYER.
 * Otherwise, the login is successful, the CLIENT is marked as "logged in"
 * and a reference to the PLAYER is retained by it.  In this case,
 * the reference count of the PLAYER is incremented to account for the
 * retained reference.
 *
 * @param CLIENT  The CLIENT that is to be logged in.
 * @param PLAYER  The PLAYER that the CLIENT is to be logged in as.
 * @return 0 if the login operation is successful, otherwise -1.
 */
int client_login(CLIENT *client, PLAYER *player) {
	pthread_mutex_lock(&client->clientMutex);
	if (client->player != NULL) {
		debug("%ld: [%d] Already logged in (player %p) [%s]", pthread_self(), client->fd, client->player, player_get_name(client->player));
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	client->player = player;
	player_ref(player, "for reference being retained by client");
	pthread_mutex_unlock(&client->clientMutex);
	return 0;
}

/*
 * Log out this CLIENT.  If the client was not logged in, then it is
 * an error.  The reference to the PLAYER that the CLIENT was logged
 * in as is discarded, and its reference count is decremented.  Any
 * INVITATIONs in the client's list are revoked or declined, if
 * possible, any games in progress are resigned, and the invitations
 * are removed from the list of this CLIENT as well as its opponents'.
 *
 * @param client  The CLIENT that is to be logged out.
 * @return 0 if the client was logged in and has been successfully
 * logged out, otherwise -1.
 */
int client_logout(CLIENT *client) {
	pthread_mutex_lock(&client->clientMutex);
	if (client->player == NULL) {
		debug("%ld: Client %p is not logged in", pthread_self(), client);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	debug("%ld: Log out client %p", pthread_self(), client);
	player_unref(client->player, "becuase refrence retained by client is being released");
	INVITE_NODE *current = client->inviteHead;
	while(current != NULL) {
		INVITE_NODE *temp = current;
		current = current->next;
		if (inv_get_game(temp->invitation) != NULL) {
			client_resign_game(client, temp->id);
		} else {
			if (inv_get_source(temp->invitation) == client) {
				client_revoke_invitation(client, temp->id);
			} else {
				client_decline_invitation(client, temp->id);
			}
		}
	}
	client->player = NULL;
	pthread_mutex_unlock(&client->clientMutex);
	return 0;
}

/*
 * Get the PLAYER for the specified logged-in CLIENT.
 * The reference count on the returned PLAYER is NOT incremented,
 * so the returned reference should only be regarded as valid as long
 * as the CLIENT has not been freed.
 *
 * @param client  The CLIENT from which to get the PLAYER.
 * @return  The PLAYER that the CLIENT is currently logged in as,
 * otherwise NULL if the player is not currently logged in.
 */
PLAYER *client_get_player(CLIENT *client) {
	pthread_mutex_lock(&client->clientMutex);
	PLAYER *player = client->player;
	pthread_mutex_unlock(&client->clientMutex);
	return player;
}

/*
 * Get the file descriptor for the network connection associated with
 * this CLIENT.
 *
 * @param client  The CLIENT for which the file descriptor is to be
 * obtained.
 * @return the file descriptor.
 */
int client_get_fd(CLIENT *client) {
	pthread_mutex_lock(&client->clientMutex);
	int fd = client->fd;
	pthread_mutex_unlock(&client->clientMutex);
	return fd;
}

INVITE_NODE *get_invite_node_from_id(CLIENT *client, int id) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = client->inviteHead;
	while(current != NULL && current->id != id) {
		current = current->next;
	}
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return NULL;
	}
	pthread_mutex_unlock(&client->clientMutex);
	return current;
}

INVITE_NODE *get_invite_node_from_inv(CLIENT *client, INVITATION *inv) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = client->inviteHead;
	while(current != NULL && current->invitation != inv) {
		current = current->next;
	}
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return NULL;
	}
	pthread_mutex_unlock(&client->clientMutex);
	return current;
}

/*
 * Send a packet to a client.  Exclusive access to the network connection
 * is obtained for the duration of this operation, to prevent concurrent
 * invocations from corrupting each other's transmissions.  To prevent
 * such interference, only this function should be used to send packets to
 * the client, rather than the lower-level proto_send_packet() function.
 *
 * @param client  The CLIENT who should be sent the packet.
 * @param pkt  The header of the packet to be sent.
 * @param data  Data payload to be sent, or NULL if none.
 * @return 0 if transmission succeeds, -1 otherwise.
 */
int client_send_packet(CLIENT *player, JEUX_PACKET_HEADER *pkt, void *data) {
	pthread_mutex_lock(&player->clientMutex);
	pkt->size = htons(pkt->size);
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
	   pkt->timestamp_sec = htonl(0);
	   pkt->timestamp_nsec = htonl(0);
	} else {
		pkt->timestamp_sec = htonl(ts.tv_sec);
		pkt->timestamp_nsec = htonl(ts.tv_nsec);
	}
	int error = proto_send_packet(player->fd, pkt, data);
	free(pkt);
	pthread_mutex_unlock(&player->clientMutex);
	return error;
}


/*
typedef struct jeux_packet_header {
    uint8_t type;		   // Type of the packet
    uint8_t id;			   // Invitation ID
    uint8_t role;		   // Role of player in game
    uint16_t size;                 // Payload size (zero if no payload)
    uint32_t timestamp_sec;        // Seconds field of time packet was sent
    uint32_t timestamp_nsec;       // Nanoseconds field of time packet was sent
} JEUX_PACKET_HEADER;
*/
/*
 * Send an ACK packet to a client.  This is a convenience function that
 * streamlines a common case.
 *
 * @param client  The CLIENT who should be sent the packet.
 * @param data  Pointer to the optional data payload for this packet,
 * or NULL if there is to be no payload.
 * @param datalen  Length of the data payload, or 0 if there is none.
 * @return 0 if transmission succeeds, -1 otherwise.
 */
int client_send_ack(CLIENT *client, void *data, size_t datalen) {
	pthread_mutex_lock(&client->clientMutex);
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt->type = JEUX_ACK_PKT;
	pkt->id = 0;
	pkt->role = 0;
	pkt->size = datalen;
	int error = client_send_packet(client, pkt, data);
	pthread_mutex_unlock(&client->clientMutex);
	return error;
}

/*
 * Send an NACK packet to a client.  This is a convenience function that
 * streamlines a common case.
 *
 * @param client  The CLIENT who should be sent the packet.
 * @return 0 if transmission succeeds, -1 otherwise.
 */
int client_send_nack(CLIENT *client) {
	pthread_mutex_lock(&client->clientMutex);
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt->type = JEUX_NACK_PKT;
	pkt->id = 0;
	pkt->role = 0;
	pkt->size = 0;
	int error = client_send_packet(client, pkt, NULL);
	pthread_mutex_unlock(&client->clientMutex);
	return error;
}

/*
 * Add an INVITATION to the list of outstanding invitations for a
 * specified CLIENT.  A reference to the INVITATION is retained by
 * the CLIENT and the reference count of the INVITATION is
 * incremented.  The invitation is assigned an integer ID,
 * which the client subsequently uses to identify the invitation.
 *
 * @param client  The CLIENT to which the invitation is to be added.
 * @param inv  The INVITATION that is to be added.
 * @return  The ID assigned to the invitation, if the invitation
 * was successfully added, otherwise -1.
 */
int client_add_invitation(CLIENT *client, INVITATION *inv) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *node = calloc(1, sizeof(INVITE_NODE));
	node->id = client->invites;
	int id = node->id;
	client->invites = client->invites + 1;
	node->next = NULL;
	node->invitation = inv;
	if (client->inviteHead == NULL) {
		client->inviteHead = node;
	} else {
		INVITE_NODE *last = client->inviteHead;
		while(last->next != NULL) {
			last = last->next;
		}
		last->next = node;
	}
	inv_ref(inv, "for refrence being retained by the client");
	pthread_mutex_unlock(&client->clientMutex);
	return id;
}

/*
 * Remove an invitation from the list of outstanding invitations
 * for a specified CLIENT.  The reference count of the invitation is
 * decremented to account for the discarded reference.
 *
 * @param client  The client from which the invitation is to be removed.
 * @param inv  The invitation that is to be removed.
 * @return the CLIENT's id for the INVITATION, if it was successfully
 * removed, otherwise -1.
 */
int client_remove_invitation(CLIENT *client, INVITATION *inv) {
	pthread_mutex_lock(&client->clientMutex);
	if (client->inviteHead != NULL && client->inviteHead->invitation == inv) {
		inv_unref(client->inviteHead->invitation, "because invitation is being removed from clients list");
		INVITE_NODE *temp = client->inviteHead;
		client->inviteHead = client->inviteHead->next;
		client->invites = client->invites - 1;
		int id = temp->id;
		free(temp);
		pthread_mutex_unlock(&client->clientMutex);
		return id;
	}
	INVITE_NODE *current = client->inviteHead;
	INVITE_NODE *prev;
	while (current != NULL && current->invitation != inv) {
		prev = current;
		current = current->next;
	}
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	client->invites = client->invites - 1;
	prev->next = current->next;
	inv_unref(current->invitation, "because invitation is being removed from clients list");
	int id = current->id;
	free(current);
	pthread_mutex_unlock(&client->clientMutex);
	return id;
}

/*
 * Make a new invitation from a specified "source" CLIENT to a specified
 * target CLIENT.  The invitation represents an offer to the target to
 * engage in a game with the source.  The invitation is added to both the
 * source's list of invitations and the target's list of invitations and
 * the invitation's reference count is appropriately increased.
 * An `INVITED` packet is sent to the target of the invitation.
 *
 * @param source  The CLIENT that is the source of the INVITATION.
 * @param target  The CLIENT that is the target of the INVITATION.
 * @param source_role  The GAME_ROLE to be played by the source of the INVITATION.
 * @param target_role  The GAME_ROLE to be played by the target of the INVITATION.
 * @return the ID assigned by the source to the INVITATION, if the operation
 * is successful, otherwise -1.
 */
int client_make_invitation(CLIENT *source, CLIENT *target, GAME_ROLE source_role, GAME_ROLE target_role) {
	INVITATION *inv = inv_create(source, target, source_role, target_role);
	if (inv == NULL) {
		return -1;
	}
	pthread_mutex_lock(&source->clientMutex);
	pthread_mutex_lock(&target->clientMutex);
	client_add_invitation(source, inv);
	int id = client_add_invitation(target, inv);
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	char *name = player_get_name(client_get_player(source));
	pkt->type = JEUX_INVITED_PKT;
	pkt->id = id;
	pkt->role = inv_get_target_role(inv);
	pkt->size = strlen(name);
	int error = client_send_packet(target, pkt, name);
	pthread_mutex_unlock(&target->clientMutex);
	pthread_mutex_unlock(&source->clientMutex);
	return error;
}

/*
 * Revoke an invitation for which the specified CLIENT is the source.
 * The invitation is removed from the lists of invitations of its source
 * and target CLIENT's and the reference counts are appropriately
 * decreased.  It is an error if the specified CLIENT is not the source
 * of the INVITATION, or the INVITATION does not exist in the source or
 * target CLIENT's list.  It is also an error if the INVITATION being
 * revoked is in a state other than the "open" state.  If the invitation
 * is successfully revoked, then the target is sent a REVOKED packet
 * containing the target's ID of the revoked invitation.
 *
 * @param client  The CLIENT that is the source of the invitation to be
 * revoked.
 * @param id  The ID assigned by the CLIENT to the invitation to be
 * revoked.
 * @return 0 if the invitation is successfully revoked, otherwise -1.
 */
int client_revoke_invitation(CLIENT *client, int id) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = get_invite_node_from_id(client, id);
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITATION *inv = current->invitation;
	if (inv_get_game(inv) != NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	CLIENT *target = inv_get_target(inv);
	pthread_mutex_lock(&target->clientMutex);
	INVITE_NODE *targetNode = get_invite_node_from_inv(target, inv);
	if (targetNode == NULL) {
		pthread_mutex_unlock(&target->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	if (inv_get_source(inv) != client) {
		pthread_mutex_unlock(&target->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	if (inv_close(inv, NULL_ROLE) == -1) {
		pthread_mutex_unlock(&target->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int error = client_remove_invitation(client, inv);
	if (error == -1) {
		pthread_mutex_unlock(&target->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int targetId = client_remove_invitation(target, inv);
	if (targetId == -1) {
		pthread_mutex_unlock(&target->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	inv_unref(inv, "because pointer to closed invitation is being discarded");
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt->type = JEUX_REVOKED_PKT;
	pkt->id = targetId;
	pkt->role = 0;
	pkt->size = 0;
	error = client_send_packet(target, pkt, NULL);
	pthread_mutex_unlock(&target->clientMutex);
	pthread_mutex_unlock(&client->clientMutex);
	return error;
}

/*
 * Decline an invitation previously made with the specified CLIENT as target.
 * The invitation is removed from the lists of invitations of its source
 * and target CLIENT's and the reference counts are appropriately
 * decreased.  It is an error if the specified CLIENT is not the target
 * of the INVITATION, or the INVITATION does not exist in the source or
 * target CLIENT's list.  It is also an error if the INVITATION being
 * declined is in a state other than the "open" state.  If the invitation
 * is successfully declined, then the source is sent a DECLINED packet
 * containing the source's ID of the declined invitation.
 *
 * @param client  The CLIENT that is the target of the invitation to be
 * declined.
 * @param id  The ID assigned by the CLIENT to the invitation to be
 * declined.
 * @return 0 if the invitation is successfully declined, otherwise -1.
 */
int client_decline_invitation(CLIENT *client, int id) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = get_invite_node_from_id(client, id);
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITATION *inv = current->invitation;
	if (inv_get_game(inv) != NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	CLIENT *source = inv_get_source(inv);
	pthread_mutex_lock(&source->clientMutex);
	INVITE_NODE *sourceNode = get_invite_node_from_inv(source, inv);
	if (sourceNode == NULL) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	if (inv_get_target(inv) != client) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	if (inv_close(inv, NULL_ROLE) == -1) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int error = client_remove_invitation(client, inv);
	if (error == -1) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int sourceId = client_remove_invitation(source, inv);
	if (sourceId == -1) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	debug("0");
	inv_unref(inv, "because pointer to closed invitation is being discarded");
	debug("1");
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt->type = JEUX_DECLINED_PKT;
	pkt->id = sourceId;
	pkt->role = 0;
	pkt->size = 0;
	debug("2");
	error = client_send_packet(source, pkt, NULL);
	debug("3");
	pthread_mutex_unlock(&source->clientMutex);
	pthread_mutex_unlock(&client->clientMutex);
	return error;
}

/*
 * Accept an INVITATION previously made with the specified CLIENT as
 * the target.  A new GAME is created and a reference to it is saved
 * in the INVITATION.  If the invitation is successfully accepted,
 * the source is sent an ACCEPTED packet containing the source's ID
 * of the accepted INVITATION.  If the source is to play the role of
 * the first player, then the payload of the ACCEPTED packet contains
 * a string describing the initial game state.  A reference to the
 * new GAME (with its reference count incremented) is returned to the
 * caller.
 *
 * @param client  The CLIENT that is the target of the INVITATION to be
 * accepted.
 * @param id  The ID assigned by the target to the INVITATION.
 * @param strp  Pointer to a variable into which will be stored either
 * NULL, if the accepting client is not the first player to move,
 * or a malloc'ed string that describes the initial game state,
 * if the accepting client is the first player to move.
 * If non-NULL, this string should be used as the payload of the `ACK`
 * message to be sent to the accepting client.  The caller must free
 * the string after use.
 * @return 0 if the INVITATION is successfully accepted, otherwise -1.
 */
int client_accept_invitation(CLIENT *client, int id, char **strp) {
	pthread_mutex_lock(&client->clientMutex);
	*strp = NULL;
	INVITE_NODE *current = get_invite_node_from_id(client, id);
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITATION *targetInv = current->invitation;
	if (inv_get_game(targetInv) != NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	CLIENT *source = inv_get_source(targetInv);
	pthread_mutex_lock(&source->clientMutex);
	current = get_invite_node_from_inv(source, targetInv);
	if (current == NULL) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int sourceId = current->id;
	int error = inv_accept(targetInv);
	if (error == -1) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	GAME_ROLE sourceRole = inv_get_source_role(targetInv);
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	char *gameState = NULL;
	int len = 0;
	*strp = NULL;
	if (sourceRole == FIRST_PLAYER_ROLE) {
		gameState = game_unparse_state(inv_get_game(targetInv));
		len = strlen(gameState);
	} else {
		*strp = game_unparse_state(inv_get_game(targetInv));
	}
	pkt->type = JEUX_ACCEPTED_PKT;
	pkt->id = sourceId;
	pkt->role = 0;
	pkt->size = len;
	error = client_send_packet(source, pkt, gameState);
	if (*strp == NULL) {
		free(gameState);
	}
	if (error == -1) {
		pthread_mutex_unlock(&source->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	pthread_mutex_unlock(&source->clientMutex);
	pthread_mutex_unlock(&client->clientMutex);
	return 0;
}

/*
 * Resign a game in progress.  This function may be called by a CLIENT
 * that is either source or the target of the INVITATION containing the
 * GAME that is to be resigned.  It is an error if the INVITATION containing
 * the GAME is not in the ACCEPTED state.  If the game is successfully
 * resigned, the INVITATION is set to the CLOSED state, it is removed
 * from the lists of both the source and target, and a RESIGNED packet
 * containing the opponent's ID for the INVITATION is sent to the opponent
 * of the CLIENT that has resigned.
 *
 * @param client  The CLIENT that is resigning.
 * @param id  The ID assigned by the CLIENT to the INVITATION that contains
 * the GAME to be resigned.
 * @return 0 if the game is successfully resigned, otherwise -1.
 */
int client_resign_game(CLIENT *client, int id) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = get_invite_node_from_id(client, id);
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITATION *inv = current->invitation;
	if (inv_get_game(inv) == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int sourceResign;
	CLIENT *opponent;
	if (client == inv_get_source(inv)) {
		sourceResign = 1;
		opponent = inv_get_target(inv);
		pthread_mutex_lock(&opponent->clientMutex);
	} else {
		sourceResign = 0;
		opponent = inv_get_source(inv);
		pthread_mutex_lock(&opponent->clientMutex);
	}
	INVITE_NODE *opponentNode = get_invite_node_from_inv(opponent, inv);
	if (opponentNode == NULL) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int error;
	if (sourceResign) {
		error = inv_close(inv, inv_get_source_role(inv));
	} else {
		error = inv_close(inv, inv_get_target_role(inv));
	}
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int clientId = client_remove_invitation(client, inv);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int opponentId = client_remove_invitation(opponent, inv);
	if (opponentId == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	player_post_result(client_get_player(client), client_get_player(opponent), 2);
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt->type = JEUX_RESIGNED_PKT;
	pkt->id = opponentId;
	pkt->role = 0;
	pkt->size = 0;
	error = client_send_packet(opponent, pkt, NULL);
	if (error == -1) {
		inv_unref(inv, "because pointer to closed invitation is being discarded");
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	GAME_ROLE winner = game_get_winner(inv_get_game(inv));
	int roleWinner;
	if (winner == FIRST_PLAYER_ROLE) {
		roleWinner = 1;
	} else if (winner == SECOND_PLAYER_ROLE) {
		roleWinner = 2;
	} else {
		roleWinner = 0;
	}
	JEUX_PACKET_HEADER *pkt1 = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt1->type = JEUX_ENDED_PKT;
	pkt1->id = clientId;
	pkt1->role = roleWinner;
	pkt1->size = 0;
	error = client_send_packet(client, pkt1, NULL);
	if (error == -1) {
		inv_unref(inv, "because pointer to closed invitation is being discarded");
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	JEUX_PACKET_HEADER *pkt2 = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt2->type = JEUX_ENDED_PKT;
	pkt2->id = opponentId;
	pkt2->role = roleWinner;
	pkt2->size = 0;
	error = client_send_packet(opponent, pkt2, NULL);
	if (error == -1) {
		inv_unref(inv, "because pointer to closed invitation is being discarded");
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	inv_unref(inv, "because pointer to closed invitation is being discarded");
	pthread_mutex_unlock(&opponent->clientMutex);
	pthread_mutex_unlock(&client->clientMutex);
	return 0;
}

/*
 * Make a move in a game currently in progress, in which the specified
 * CLIENT is a participant.  The GAME in which the move is to be made is
 * specified by passing the ID assigned by the CLIENT to the INVITATION
 * that contains the game.  The move to be made is specified as a string
 * that describes the move in a game-dependent format.  It is an error
 * if the ID does not refer to an INVITATION containing a GAME in progress,
 * if the move cannot be parsed, or if the move is not legal in the current
 * GAME state.  If the move is successfully made, then a MOVED packet is
 * sent to the opponent of the CLIENT making the move.  In addition, if
 * the move that has been made results in the game being over, then an
 * ENDED packet containing the appropriate game ID and the game result
 * is sent to each of the players participating in the game, and the
 * INVITATION containing the now-terminated game is removed from the lists
 * of both the source and target.  The result of the game is posted in
 * order to update both players' ratings.
 *
 * @param client  The CLIENT that is making the move.
 * @param id  The ID assigned by the CLIENT to the GAME in which the move
 * is to be made.
 * @param move  A string that describes the move to be made.
 * @return 0 if the move was made successfully, -1 otherwise.
 */
int client_make_move(CLIENT *client, int id, char *move) {
	pthread_mutex_lock(&client->clientMutex);
	INVITE_NODE *current = get_invite_node_from_id(client, id);
	if (current == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITATION *inv = current->invitation;
	if (inv_get_game(inv) == NULL) {
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	GAME_ROLE clientRole;
	CLIENT *opponent;
	if (client == inv_get_source(inv)) {
		clientRole = inv_get_source_role(inv);
		opponent = inv_get_target(inv);
		pthread_mutex_lock(&opponent->clientMutex);
	} else {
		clientRole = inv_get_target_role(inv);
		opponent = inv_get_source(inv);
		pthread_mutex_lock(&opponent->clientMutex);
	}
	GAME_MOVE *gameMove = game_parse_move(inv_get_game(inv), clientRole, move);
	if (gameMove == NULL) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	int error = game_apply_move(inv_get_game(inv), gameMove);
	free(gameMove);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	INVITE_NODE *opponentNode = get_invite_node_from_inv(opponent, inv);
	if (opponentNode == NULL) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	JEUX_PACKET_HEADER *pkt = calloc(1, sizeof(JEUX_PACKET_HEADER));
	char *gameState = game_unparse_state(inv_get_game(inv));
	char *buffer = calloc(1024, sizeof(char));
	buffer[0] = '\n';
	strcat(buffer, gameState);
	free(gameState);
	if (!game_is_over(inv_get_game(inv))) {
		if (clientRole == FIRST_PLAYER_ROLE) {
			strcat(buffer, "\nO to move\n");
		} else {
			strcat(buffer, "\nX to move\n");
		}
	}
	pkt->type = JEUX_MOVED_PKT;
	pkt->id = opponentNode->id;
	pkt->role = 0;
	pkt->size = strlen(buffer);
	error = client_send_packet(opponent, pkt, buffer);
	free(buffer);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	if (!game_is_over(inv_get_game(inv))) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return 0;
	}
	GAME_ROLE winner = game_get_winner(inv_get_game(inv));
	int clientOutcome;
	if (winner == clientRole) {
		clientOutcome = 1;
	} else if (winner == NULL_ROLE) {
		clientOutcome = 0;
	} else {
		clientOutcome = 2;
	}
	int roleWinner;
	if (winner == FIRST_PLAYER_ROLE) {
		roleWinner = 1;
	} else if (winner == SECOND_PLAYER_ROLE) {
		roleWinner = 2;
	} else {
		roleWinner = 0;
	}
	JEUX_PACKET_HEADER *pkt1 = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt1->type = JEUX_ENDED_PKT;
	pkt1->id = current->id;
	pkt1->role = roleWinner;
	pkt1->size = 0;
	error = client_send_packet(client, pkt1, NULL);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	JEUX_PACKET_HEADER *pkt2 = calloc(1, sizeof(JEUX_PACKET_HEADER));
	pkt2->type = JEUX_ENDED_PKT;
	pkt2->id = opponentNode->id;
	pkt2->role = roleWinner;
	pkt2->size = 0;
	error = client_send_packet(opponent, pkt2, NULL);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	error = inv_close(inv, winner);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	player_post_result(client->player, opponent->player, clientOutcome);
	error = client_remove_invitation(client, inv);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	error = client_remove_invitation(opponent, inv);
	if (error == -1) {
		pthread_mutex_unlock(&opponent->clientMutex);
		pthread_mutex_unlock(&client->clientMutex);
		return -1;
	}
	inv_unref(inv, "because pointer to closed invitation is being discarded");
	pthread_mutex_unlock(&opponent->clientMutex);
	pthread_mutex_unlock(&client->clientMutex);
	return 0;
}
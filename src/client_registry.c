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

#include "client_registry.h"
#include "csapp.h"
#include "debug.h"

int waitingForEmpty = 0;

struct client_registry {
    int numClients;
    CLIENT *clients[MAX_CLIENTS];
    sem_t registryMutex;
    sem_t emptyRegisterMutex;
};

/*
 * Initialize a new client registry.
 *
 * @return  the newly initialized client registry, or NULL if initialization
 * fails.
 */
CLIENT_REGISTRY *creg_init() {
	CLIENT_REGISTRY *registry = malloc(sizeof(CLIENT_REGISTRY));
	registry->numClients = 0;
	Sem_init(&registry->registryMutex, 0, 1);
	Sem_init(&registry->emptyRegisterMutex, 0, 1);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		registry->clients[i] = NULL;
	}
	debug("%ld: Initialize client registry", pthread_self());
	return registry;
}

/*
 * Finalize a client registry, freeing all associated resources.
 * This method should not be called unless there are no currently
 * registered clients.
 *
 * @param cr  The client registry to be finalized, which must not
 * be referenced again.
 */
void creg_fini(CLIENT_REGISTRY *cr) {
	P(&cr->registryMutex);
	debug("%ld: Finalize client registry", pthread_self());
	if (cr->clients != 0) {
		V(&cr->registryMutex);
		free(cr);
		return;
	} else {
		for(int i = 0; i < MAX_CLIENTS; i++) {
			cr->clients[i] = NULL;
		}
		cr->numClients = 0;
		free(cr);
		return;
	}
}

/*
 * Register a client file descriptor.
 * If successful, returns a reference to the the newly registered CLIENT,
 * otherwise NULL.  The returned CLIENT has a reference count of one;
 * this corresponds to the reference held by the registry itself for as
 * long as the client remains connected.
 *
 * @param cr  The client registry.
 * @param fd  The file descriptor to be registered.
 * @return a reference to the newly registered CLIENT, if registration
 * is successful, otherwise NULL.
 */
CLIENT *creg_register(CLIENT_REGISTRY *cr, int fd) {
	P(&cr->registryMutex);
	if (cr->numClients == MAX_CLIENTS) {
		V(&cr->registryMutex);
		return NULL;
	}
	CLIENT *client = client_create(cr, fd);
	if (client == NULL) {
		V(&cr->registryMutex);
		return NULL;
	}
	for(int i = 0; i < MAX_CLIENTS; i++) {
		if (cr->clients[i] == NULL) {
			cr->clients[i] = client;
			cr->numClients = cr->numClients + 1;
			debug("%ld: Register client fd %d (total connected: %d)", pthread_self(), fd, cr->numClients);
			break;
		}
	}
	V(&cr->registryMutex);
	return client;
}

/*
 * Unregister a CLIENT, removing it from the registry.
 * The client reference count is decreased by one to account for the
 * pointer discarded by the client registry.  If the number of registered
 * clients is now zero, then any threads that are blocked in
 * creg_wait_for_empty() waiting for this situation to occur are allowed
 * to proceed.  It is an error if the CLIENT is not currently registered
 * when this function is called.
 *
 * @param cr  The client registry.
 * @param client  The CLIENT to be unregistered.
 * @return 0  if unregistration succeeds, otherwise -1.
 */
int creg_unregister(CLIENT_REGISTRY *cr, CLIENT *client) {
	P(&cr->registryMutex);
	int found = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (cr->clients[i] == client) {
			cr->clients[i] = NULL;
			cr->numClients = cr->numClients - 1;
			found = 1;
			debug("%ld: Unregister client %d (total connected: %d)", pthread_self(), client_get_fd(client), cr->numClients);
			client_unref(client, "because client is being unregistered");
			break;
		}
	}
	if (found) {
		V(&cr->registryMutex);
		if (cr->numClients == 0 && waitingForEmpty) {
			V(&cr->emptyRegisterMutex);
		}
		return 0;
	} else {
		V(&cr->registryMutex);
		if (cr->numClients == 0 && waitingForEmpty) {
			V(&cr->emptyRegisterMutex);
		}
		return -1;
	}
}

/*
 * Given a username, return the CLIENT that is logged in under that
 * username.  The reference count of the returned CLIENT is
 * incremented by one to account for the reference returned.
 *
 * @param cr  The registry in which the lookup is to be performed.
 * @param user  The username that is to be looked up.
 * @return the CLIENT currently registered under the specified
 * username, if there is one, otherwise NULL.
 */
CLIENT *creg_lookup(CLIENT_REGISTRY *cr, char *user) {
	P(&cr->registryMutex);;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (cr->clients[i] != NULL) {
			PLAYER *player = client_get_player(cr->clients[i]);
			if (player != NULL) {
				char *username = player_get_name(player);
				if (strcmp(username, user) == 0) {
					client_ref(cr->clients[i], "for reference being returned by creg_lookup()");
					V(&cr->registryMutex);
					return cr->clients[i];
				}
			}
		}
	}
	V(&cr->registryMutex);
	return NULL;
}

/*
 * Return a list of all currently logged in players.  The result is
 * returned as a malloc'ed array of PLAYER pointers, with a NULL
 * pointer marking the end of the array.  It is the caller's
 * responsibility to decrement the reference count of each of the
 * entries and to free the array when it is no longer needed.
 *
 * @param cr  The registry for which the set of players is to be
 * obtained.
 * @return the list of players.
 */
PLAYER **creg_all_players(CLIENT_REGISTRY *cr) {
	P(&cr->registryMutex);
	int maxPlayers = 10;
	int numPlayers = 0;
	PLAYER **playerList = (PLAYER **)malloc(maxPlayers * sizeof(PLAYER *));
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (cr->clients[i] != NULL) {
			PLAYER *player = client_get_player(cr->clients[i]);
			if (player != NULL) {
				playerList[numPlayers++] = player;
				player_ref(player, "for reference being added to players list");
				if (numPlayers + 1 == maxPlayers) {
					maxPlayers = maxPlayers + 10;
					playerList = realloc(playerList, maxPlayers);
				}
			}
		}
	}
	playerList[numPlayers] = NULL;
	V(&cr->registryMutex);
	return playerList;
}

/*
 * A thread calling this function will block in the call until
 * the number of registered clients has reached zero, at which
 * point the function will return.  Note that this function may be
 * called concurrently by an arbitrary number of threads.
 *
 * @param cr  The client registry.
 */
void creg_wait_for_empty(CLIENT_REGISTRY *cr) {
	if (cr->numClients != 0) {
		P(&cr->emptyRegisterMutex);
		waitingForEmpty = 1;
		P(&cr->emptyRegisterMutex);
		return;
	} else {
		return;
	}
}

/*
 * Shut down (using shutdown(2)) all the sockets for connections
 * to currently registered clients.  The clients are not unregistered
 * by this function.  It is intended that the clients will be
 * unregistered by the threads servicing their connections, once
 * those server threads have recognized the EOF on the connection
 * that has resulted from the socket shutdown.
 *
 * @param cr  The client registry.
 */
void creg_shutdown_all(CLIENT_REGISTRY *cr) {
	P(&cr->registryMutex);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (cr->clients[i] != NULL) {
			int fd = client_get_fd(cr->clients[i]);
			debug("%ld: Shutting down client %d", pthread_self(), fd);
			shutdown(fd, SHUT_WR);
		}
	}
	V(&cr->registryMutex);
}
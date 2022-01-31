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

#include "player_registry.h"
#include "csapp.h"
#include "debug.h"

typedef struct player_node {
	PLAYER *player;
	struct player_node *next;
} PLAYER_NODE;

struct player_registry {
    PLAYER_NODE* head;
    sem_t registryMutex;
};

/*
 * Initialize a new player registry.
 *
 * @return the newly initialized PLAYER_REGISTRY, or NULL if initialization
 * fails.
 */
PLAYER_REGISTRY *preg_init(void) {
	PLAYER_REGISTRY *registry = malloc(sizeof(PLAYER_REGISTRY));
	Sem_init(&registry->registryMutex, 0, 1);
	registry->head = NULL;
	debug("%ld: Initialize player registry", pthread_self());
	return registry;
}

/*
 * Finalize a player registry, freeing all associated resources.
 *
 * @param cr  The PLAYER_REGISTRY to be finalized, which must not
 * be referenced again.
 */
void preg_fini(PLAYER_REGISTRY *preg) {
	P(&preg->registryMutex);
	PLAYER_NODE *current = preg->head;
	while (current != NULL) {
		PLAYER_NODE *temp = current;
		current = current->next;
		player_unref(temp->player, "becuase player registry is being finalized");
		free(temp);
	}
	free(preg);
	return;
}

/*
 * Register a player with a specified user name.  If there is already
 * a player registered under that user name, then the existing registered
 * player is returned, otherwise a new player is created.
 * If an existing player is returned, then its reference count is increased
 * by one to account for the returned pointer.  If a new player is
 * created, then the returned player has reference count equal to two:
 * one count for the pointer retained by the registry and one count for
 * the pointer returned to the caller.
 *
 * @param name  The player's user name, which is copied by this function.
 * @return A pointer to a PLAYER object, in case of success, otherwise NULL.
 *
 */
PLAYER *preg_register(PLAYER_REGISTRY *preg, char *name) {
	P(&preg->registryMutex);
	PLAYER_NODE *current = preg->head;
	while (current != NULL && strcmp(player_get_name(current->player), name) != 0) {
		current = current->next;
	}
	if (current == NULL) {
		PLAYER *player = player_create(name);
		PLAYER_NODE *node = malloc(sizeof(PLAYER_NODE));
		node->player = player;
		node->next = preg->head;
		preg->head = node;
		player_ref(player, "for refrence being retained by player registry");
		V(&preg->registryMutex);
		return player;
	} else {
		player_ref(current->player, "for new refrence to existing player");
		V(&preg->registryMutex);
		return current->player;
	}
}

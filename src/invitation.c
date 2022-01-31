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

#include "client_registry.h"
#include "game.h"
#include "csapp.h"
#include "invitation.h"
#include "debug.h"

struct invitation {
	CLIENT *source;
	GAME_ROLE source_role;
	CLIENT *target;
	GAME_ROLE target_role;
	INVITATION_STATE state;
	GAME *game;
	int count;
	sem_t invitationMutex;
};

/*
 * Create an INVITATION in the OPEN state, containing reference to
 * specified source and target CLIENTs, which cannot be the same CLIENT.
 * The reference counts of the source and target are incremented to reflect
 * the stored references.
 *
 * @param source  The CLIENT that is the source of this INVITATION.
 * @param target  The CLIENT that is the target of this INVITATION.
 * @param source_role  The GAME_ROLE to be played by the source of this INVITATION.
 * @param target_role  The GAME_ROLE to be played by the target of this INVITATION.
 * @return a reference to the newly created INVITATION, if initialization
 * was successful, otherwise NULL.
 */
INVITATION *inv_create(CLIENT *source, CLIENT *target, GAME_ROLE source_role, GAME_ROLE target_role) {
	if (source == target) {
		debug("%ld: Source and target cannot be the same client", pthread_self());
		return NULL;
	}
	INVITATION *invitation = malloc(sizeof(INVITATION));
	invitation->source = source;
	invitation->target = target;
	invitation->source_role = source_role;
	invitation->target_role = target_role;
	invitation->state = INV_OPEN_STATE;
	invitation->count = 0;
	invitation->game = NULL;
	Sem_init(&invitation->invitationMutex, 0, 1);
	client_ref(source, "as source of new invitation");
	client_ref(target, "as target of new invitation");
	inv_ref(invitation, "for newly created invitation");
	return invitation;
}

/*
 * Increase the reference count on an invitation by one.
 *
 * @param inv  The INVITATION whose reference count is to be increased.
 * @param why  A string describing the reason why the reference count is
 * being increased.  This is used for debugging printout, to help trace
 * the reference counting.
 * @return  The same INVITATION object that was passed as a parameter.
 */
INVITATION *inv_ref(INVITATION *inv, char *why) {
	P(&inv->invitationMutex);
	debug("%ld: Increase refrence count on invitation %p (%d -> %d) %s", pthread_self(), inv, inv->count, inv->count + 1, why);
	inv->count = inv->count + 1;
	V(&inv->invitationMutex);
	return inv;
}

/*
 * Decrease the reference count on an invitation by one.
 * If after decrementing, the reference count has reached zero, then the
 * invitation and its contents are freed.
 *
 * @param inv  The INVITATION whose reference count is to be decreased.
 * @param why  A string describing the reason why the reference count is
 * being decreased.  This is used for debugging printout, to help trace
 * the reference counting.
 *
 */
void inv_unref(INVITATION *inv, char *why) {
	P(&inv->invitationMutex);
	debug("%ld: Decrease refrence count on invitation %p (%d -> %d) %s", pthread_self(), inv, inv->count, inv->count - 1, why);
	inv->count = inv->count - 1;
	if (inv->count == 0) {
		debug("%ld: Free invitation %p", pthread_self(), inv);
		client_unref(inv->source, "because invitation is being freed");
		client_unref(inv->target, "because invitation is being freed");
		if (inv->game != NULL) {
			game_unref(inv->game, "because invitation is being freed");
		}
		free(inv);
		return;
	}
	V(&inv->invitationMutex);
}

/*
 * Get the CLIENT that is the source of an INVITATION.
 * The reference count of the returned CLIENT is NOT incremented,
 * so the CLIENT reference should only be regarded as valid as
 * long as the INVITATION has not been freed.
 *
 * @param inv  The INVITATION to be queried.
 * @return the CLIENT that is the source of the INVITATION.
 */
CLIENT *inv_get_source(INVITATION *inv) {
	P(&inv->invitationMutex);
	CLIENT *source = inv->source;
	V(&inv->invitationMutex);
	return source;
}

/*
 * Get the CLIENT that is the target of an INVITATION.
 * The reference count of the returned CLIENT is NOT incremented,
 * so the CLIENT reference should only be regarded as valid if
 * the INVITATION has not been freed.
 *
 * @param inv  The INVITATION to be queried.
 * @return the CLIENT that is the target of the INVITATION.
 */
CLIENT *inv_get_target(INVITATION *inv) {
	P(&inv->invitationMutex);
	CLIENT *target = inv->target;
	V(&inv->invitationMutex);
	return target;
}

/*
 * Get the GAME_ROLE to be played by the source of an INVITATION.
 *
 * @param inv  The INVITATION to be queried.
 * @return the GAME_ROLE played by the source of the INVITATION.
 */
GAME_ROLE inv_get_source_role(INVITATION *inv) {
	P(&inv->invitationMutex);
	GAME_ROLE source_role = inv->source_role;
	V(&inv->invitationMutex);
	return source_role;
}

/*
 * Get the GAME_ROLE to be played by the target of an INVITATION.
 *
 * @param inv  The INVITATION to be queried.
 * @return the GAME_ROLE played by the target of the INVITATION.
 */
GAME_ROLE inv_get_target_role(INVITATION *inv) {
	P(&inv->invitationMutex);
	GAME_ROLE target_role = inv->target_role;
	V(&inv->invitationMutex);
	return target_role;
}

/*
 * Get the GAME (if any) associated with an INVITATION.
 * The reference count of the returned GAME is NOT incremented,
 * so the GAME reference should only be regarded as valid as long
 * as the INVITATION has not been freed.
 *
 * @param inv  The INVITATION to be queried.
 * @return the GAME associated with the INVITATION, if there is one,
 * otherwise NULL.
 */
GAME *inv_get_game(INVITATION *inv) {
	P(&inv->invitationMutex);
	GAME *game = NULL;
	if (inv->game != NULL) {
		game = inv->game;
	}
	V(&inv->invitationMutex);
	return game;
}

/*
 * Accept an INVITATION, changing it from the OPEN to the
 * ACCEPTED state, and creating a new GAME.  If the INVITATION was
 * not previously in the the OPEN state then it is an error.
 *
 * @param inv  The INVITATION to be accepted.
 * @return 0 if the INVITATION was successfully accepted, otherwise -1.
 */
int inv_accept(INVITATION *inv) {
	P(&inv->invitationMutex);
	if (inv->state != INV_OPEN_STATE) {
		V(&inv->invitationMutex);
		return -1;
	} else {
		inv->state = INV_ACCEPTED_STATE;
		inv->game = game_create();
	}
	V(&inv->invitationMutex);
	return 0;
}

/*
 * Close an INVITATION, changing it from either the OPEN state or the
 * ACCEPTED state to the CLOSED state.  If the INVITATION was not previously
 * in either the OPEN state or the ACCEPTED state, then it is an error.
 * If an INVITATION that has a GAME in progress is closed, then the GAME
 * will be resigned by a specified player.
 *
 * @param inv  The INVITATION to be closed.
 * @param role  This parameter identifies the GAME_ROLE of the player that
 * should resign as a result of closing an INVITATION that has a game in
 * progress.  If NULL_ROLE is passed, then the invitation can only be
 * closed if there is no game in progress.
 * @return 0 if the INVITATION was successfully closed, otherwise -1.
 */
int inv_close(INVITATION *inv, GAME_ROLE role) {
	P(&inv->invitationMutex);
	if (inv->state != INV_OPEN_STATE && inv->state != INV_ACCEPTED_STATE) {
		V(&inv->invitationMutex);
		return -1;
	} else {
		if (inv->game == NULL) {
			inv->state = INV_CLOSED_STATE;
		} else {
			if (game_is_over(inv->game)) {
				inv->state = INV_CLOSED_STATE;
			} else {
				if (role == NULL_ROLE) {
					V(&inv->invitationMutex);
					return -1;
				} else {
					game_resign(inv->game, role);
					inv->state = INV_CLOSED_STATE;
				}
			}
		}
	}
	V(&inv->invitationMutex);
	return 0;
}
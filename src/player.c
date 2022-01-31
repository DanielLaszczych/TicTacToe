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
#include <math.h>

#include "player.h"
#include "csapp.h"
#include "debug.h"

struct player {
	int rating;
	char* name;
	int count;
	pthread_mutex_t playerMutex;
	pthread_mutexattr_t mutexAttr;
};

/*
 * Create a new PLAYER with a specified username.  A private copy is
 * made of the username that is passed.  The newly created PLAYER has
 * a reference count of one, corresponding to the reference that is
 * returned from this function.
 *
 * @param name  The username of the PLAYER.
 * @return  A reference to the newly created PLAYER, if initialization
 * was successful, otherwise NULL.
 */
PLAYER *player_create(char *name) {
	PLAYER *player = malloc(sizeof(PLAYER));
	player->rating = PLAYER_INITIAL_RATING;
	char *temp = malloc((strlen(name) + 1) * sizeof(char));
	strcpy(temp, name);
	player->name = temp;
	player->count = 0;
	pthread_mutexattr_init(&player->mutexAttr);
	pthread_mutexattr_settype(&player->mutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&player->playerMutex, &player->mutexAttr);
	player_ref(player, "for newly created player");
	return player;
}

/*
 * Increase the reference count on a player by one.
 *
 * @param player  The PLAYER whose reference count is to be increased.
 * @param why  A string describing the reason why the reference count is
 * being increased.  This is used for debugging printout, to help trace
 * the reference counting.
 * @return  The same PLAYER object that was passed as a parameter.
 */
PLAYER *player_ref(PLAYER *player, char *why) {
	pthread_mutex_lock(&player->playerMutex);
	debug("%ld: Increase refrence count on player %p (%d -> %d) %s", pthread_self(), player, player->count, player->count + 1, why);
	player->count = player->count + 1;
	pthread_mutex_unlock(&player->playerMutex);
	return player;
}

/*
 * Decrease the reference count on a PLAYER by one.
 * If after decrementing, the reference count has reached zero, then the
 * PLAYER and its contents are freed.
 *
 * @param player  The PLAYER whose reference count is to be decreased.
 * @param why  A string describing the reason why the reference count is
 * being decreased.  This is used for debugging printout, to help trace
 * the reference counting.
 *
 */
void player_unref(PLAYER *player, char *why) {
	pthread_mutex_lock(&player->playerMutex);
	debug("%ld: Decrease refrence count on player %p (%d -> %d) %s", pthread_self(), player, player->count, player->count - 1, why);
	player->count = player->count - 1;
	if (player->count == 0) {
		debug("%ld, Free player %p", pthread_self(), player);
		free(player->name);
		free(player);
		return;
	}
	pthread_mutex_unlock(&player->playerMutex);
	return;
}

/*
 * Get the username of a player.
 *
 * @param player  The PLAYER that is to be queried.
 * @return the username of the player.
 */
char *player_get_name(PLAYER *player) {
	pthread_mutex_lock(&player->playerMutex);
	char *name = player->name;
	pthread_mutex_unlock(&player->playerMutex);
	return name;
}
/*
 * Get the rating of a player.
 *
 * @param player  The PLAYER that is to be queried.
 * @return the rating of the player.
 */
int player_get_rating(PLAYER *player) {
	pthread_mutex_lock(&player->playerMutex);
	int rating = player->rating;
	pthread_mutex_unlock(&player->playerMutex);
	return rating;
}

/*
 * Post the result of a game between two players.
 * To update ratings, we use a system of a type devised by Arpad Elo,
 * similar to that used by the US Chess Federation.
 * The player's ratings are updated as follows:
 * Assign each player a score of 0, 0.5, or 1, according to whether that
 * player lost, drew, or won the game.
 * Let S1 and S2 be the scores achieved by player1 and player2, respectively.
 * Let R1 and R2 be the current ratings of player1 and player2, respectively.
 * Let E1 = 1/(1 + 10**((R2-R1)/400)), and
 *     E2 = 1/(1 + 10**((R1-R2)/400))
 * Update the players ratings to R1' and R2' using the formula:
 *     R1' = R1 + 32*(S1-E1)
 *     R2' = R2 + 32*(S2-E2)
 *
 * @param player1  One of the PLAYERs that is to be updated.
 * @param player2  The other PLAYER that is to be updated.
 * @param result   0 if draw, 1 if player1 won, 2 if player2 won.
 */
void player_post_result(PLAYER *player1, PLAYER *player2, int result) {
	pthread_mutex_lock(&player1->playerMutex);
	pthread_mutex_lock(&player2->playerMutex);
	float S1;
	float S2;
	if (result == 0) {
		S1 = 0.5;
		S2 = 0.5;
	} else if (result == 1) {
		S1 = 1.0;
		S2 = 0.0;
	} else {
		S1 = 0.0;
		S2 = 1.0;
	}
	float R1 = player_get_rating(player1);
	float R2 = player_get_rating(player2);
	float E1 = 1.0/(1.0 + pow(10.0, ((R2-R1)/400.0)));
	float E2 = 1.0/(1.0 + pow(10.0, ((R1-R2)/400.0)));
	int newR1 = R1 + (int)(32.0 * (S1-E1));
	int newR2 = R2 + (int)(32.0 * (S2-E2));
	player1->rating = newR1;
	player2->rating = newR2;
	pthread_mutex_unlock(&player2->playerMutex);
	pthread_mutex_unlock(&player1->playerMutex);
	return;
}
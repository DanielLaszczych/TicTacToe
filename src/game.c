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


#include "game.h"
#include "csapp.h"
#include "debug.h"

struct game {
	int game_board[9];
	int isOver;
	GAME_ROLE winner;
	int count;
	int expectedPiece;
	pthread_mutex_t gameMutex;
	pthread_mutexattr_t mutexAttr;
};

struct game_move {
	int piece;
	int placement;
};

GAME *game_create(void) {
	GAME *game = calloc(1, sizeof(GAME));
	for (int i = 0; i < 9; i++) {
		game->game_board[i] = -1;
	}
	game->isOver = 0;
	game->winner = NULL_ROLE;
	game->count = 0;
	game->expectedPiece = 1;
	pthread_mutexattr_init(&game->mutexAttr);
	pthread_mutexattr_settype(&game->mutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&game->gameMutex, &game->mutexAttr);
	game_ref(game, "for newly created game");
	return game;
}

/*
 * Increase the reference count on a game by one.
 *
 * @param game  The GAME whose reference count is to be increased.
 * @param why  A string describing the reason why the reference count is
 * being increased.  This is used for debugging printout, to help trace
 * the reference counting.
 * @return  The same GAME object that was passed as a parameter.
 */
GAME *game_ref(GAME *game, char *why) {
	pthread_mutex_lock(&game->gameMutex);
	debug("%ld: Increase refrence count on game %p (%d -> %d) %s", pthread_self(), game, game->count, game->count + 1, why);
	game->count = game->count + 1;
	pthread_mutex_unlock(&game->gameMutex);
	return game;
}

/*
 * Decrease the reference count on a game by one.  If after
 * decrementing, the reference count has reached zero, then the
 * GAME and its contents are freed.
 *
 * @param game  The GAME whose reference count is to be decreased.
 * @param why  A string describing the reason why the reference count is
 * being decreased.  This is used for debugging printout, to help trace
 * the reference counting.
 */
void game_unref(GAME *game, char *why) {
	pthread_mutex_lock(&game->gameMutex);
	debug("%ld: Decrease refrence count on game %p (%d -> %d) %s", pthread_self(), game, game->count, game->count - 1, why);
	game->count = game->count - 1;
	if (game->count == 0) {
		debug("%ld: Free game %p", pthread_self(), game);
		free(game);
		return;
	}
	pthread_mutex_unlock(&game->gameMutex);
	return;
}

/*
 * Apply a GAME_MOVE to a GAME.
 * If the move is illegal in the current GAME state, then it is an error.
 *
 * @param game  The GAME to which the move is to be applied.
 * @param move  The GAME_MOVE to be applied to the game.
 * @return 0 if application of the move was successful, otherwise -1.
 */
int game_apply_move(GAME *game, GAME_MOVE *move) {
	pthread_mutex_lock(&game->gameMutex);
	if (game->isOver || game->game_board[move->placement - 1] != -1 || game->expectedPiece != move->piece) {
		pthread_mutex_unlock(&game->gameMutex);
		return -1;
	} else {
		game->game_board[move->placement - 1] = move->piece;
		game->expectedPiece = 1 - game->expectedPiece;
		if (game->game_board[0] != -1) {
			if (game->game_board[0] == game->game_board[4] && game->game_board[4] == game->game_board[8]) {
				game->isOver = 1;
				if(game->game_board[0] == 1) {
					game->winner = FIRST_PLAYER_ROLE;
				} else {
					game->winner = SECOND_PLAYER_ROLE;
				}
				pthread_mutex_unlock(&game->gameMutex);
				return 0;
			}
		}
		if (game->game_board[2] != -1) {
			if (game->game_board[2] == game->game_board[4] && game->game_board[4] == game->game_board[6]) {
				game->isOver = 1;
				if(game->game_board[2] == 1) {
					game->winner = FIRST_PLAYER_ROLE;
				} else {
					game->winner = SECOND_PLAYER_ROLE;
				}
				pthread_mutex_unlock(&game->gameMutex);
				return 0;
			}
		}
		for(int i = 0, j = 0; i < 7 && j < 3; i+=3, j++) {
			if (game->game_board[i] != -1) {
				if (game->game_board[i] == game->game_board[i + 1] && game->game_board[i + 1] == game->game_board[i + 2]) {
					game->isOver = 1;
					if(game->game_board[i] == 1) {
						game->winner = FIRST_PLAYER_ROLE;
					} else {
						game->winner = SECOND_PLAYER_ROLE;
					}
					break;
				}
			}
			if (game->game_board[j] != -1) {
				if (game->game_board[j] == game->game_board[j + 3] && game->game_board[j + 3] == game->game_board[j + 6]) {
					game->isOver = 1;
					if(game->game_board[j] == 1) {
						game->winner = FIRST_PLAYER_ROLE;
					} else {
						game->winner = SECOND_PLAYER_ROLE;
					}
					break;
				}
			}
		}
		int cleared = 1;
		if (!game->isOver) {
			for (int i = 0; i < 9; i++) {
				if (game->game_board[i] == -1) {
					cleared = 0;
				}
			}
			if (cleared) {
				game->isOver = 1;
				game->winner = NULL_ROLE;
			}
		}
		pthread_mutex_unlock(&game->gameMutex);
		return 0;
	}
}

/*
 * Submit the resignation of the GAME by the player in a specified
 * GAME_ROLE.  It is an error if the game has already terminated.
 *
 * @param game  The GAME to be resigned.
 * @param role  The GAME_ROLE of the player making the resignation.
 */
int game_resign(GAME *game, GAME_ROLE role) {
	pthread_mutex_lock(&game->gameMutex);
	if (game->isOver) {
		pthread_mutex_unlock(&game->gameMutex);
		return -1;
	}
	game->isOver = 1;
	if (role == FIRST_PLAYER_ROLE) {
		game->winner = SECOND_PLAYER_ROLE;
	} else {
		game->winner = FIRST_PLAYER_ROLE;
	}
	pthread_mutex_unlock(&game->gameMutex);
	return 0;
}

/*
 * Get a string that describes the current GAME state, in a format
 * appropriate for human users.  The returned string is in malloc'ed
 * storage, which the caller is responsible for freeing when the string
 * is no longer required.
 *
 * @param game  The GAME for which the state description is to be
 * obtained.
 * @return  A string that describes the current GAME state.
 */
char *game_unparse_state(GAME *game) {
	pthread_mutex_lock(&game->gameMutex);
	char *gameState = calloc(30, sizeof(char));
	char *temp = gameState;
	int boardPlace = 0;
	int dashes = 0;
	for(int i = 1; i < 30; i++) {
		if (dashes) {
			if (i % 6 == 0) {
				dashes = 1 - dashes;
				gameState[i - 1] = '\n';
			} else {
				gameState[i - 1] = '-';
			}
		} else {
			if (i % 6 == 0) {
				dashes = 1 - dashes;
				gameState[i - 1] = '\n';
			} else {
				if (i % 2 == 0) {
					gameState[i - 1] = '|';
				} else {
					int gamePiece = game->game_board[boardPlace++];
					if (gamePiece == -1) {
						gameState[i - 1] = ' ';
					} else if (gamePiece == 1) {
						gameState[i - 1] = 'X';
					} else {
						gameState[i - 1] = 'O';
					}
				}
			}
		}
	}
	pthread_mutex_unlock(&game->gameMutex);
	return temp;
}

/*
 * Determine if a specifed GAME has terminated.
 *
 * @param game  The GAME to be queried.
 * @return 1 if the game is over, 0 otherwise.
 */
int game_is_over(GAME *game) {
	pthread_mutex_lock(&game->gameMutex);
	int over = game->isOver;
	pthread_mutex_unlock(&game->gameMutex);
	return over;
}

/*
 * Get the GAME_ROLE of the player who has won the game.
 *
 * @param game  The GAME for which the winner is to be obtained.
 * @return  The GAME_ROLE of the winning player, if there is one.
 * If the game is not over, or there is no winner because the game
 * is drawn, then NULL_PLAYER is returned.
 */
GAME_ROLE game_get_winner(GAME *game) {
	pthread_mutex_lock(&game->gameMutex);
	GAME_ROLE winner = game->winner;
	pthread_mutex_unlock(&game->gameMutex);
	return winner;
}

/*
 * Attempt to interpret a string as a move in the specified GAME.
 * If successful, a GAME_MOVE object representing the move is returned,
 * otherwise NULL is returned.  The caller is responsible for freeing
 * the returned GAME_MOVE when it is no longer needed.
 * Refer to the assignment handout for the syntax that should be used
 * to specify a move.
 *
 * @param game  The GAME for which the move is to be parsed.
 * @param role  The GAME_ROLE of the player making the move.
 * If this is not NULL_ROLE, then it must agree with the role that is
 * currently on the move in the game.
 * @param str  The string that is to be interpreted as a move.
 * @return  A GAME_MOVE described by the given string, if the string can
 * in fact be interpreted as a move, otherwise NULL.
 */
GAME_MOVE *game_parse_move(GAME *game, GAME_ROLE role, char *str) {
	pthread_mutex_lock(&game->gameMutex);
	int placement = (int)(str[0]) - 48;
	if (placement <= 0 && placement >= 10) {
		pthread_mutex_unlock(&game->gameMutex);
		return NULL;
	}
	int piece = -1;
	for (int i = 1; i < strlen(str); i++) {
		if (str[i] == 'x' || str[i] == 'X') {
			piece = 1;
			break;
		} else if (str[i] == 'o' || str[i] == 'O') {
			piece = 0;
			break;
		}
	}
	if (piece == -1) {
		pthread_mutex_unlock(&game->gameMutex);
		return NULL;
	}
	GAME_MOVE *move = malloc(sizeof(GAME_MOVE));
	move->placement = placement;
	move->piece = piece;
	pthread_mutex_unlock(&game->gameMutex);
	return move;
}

/*
 * Get a string that describes a specified GAME_MOVE, in a format
 * appropriate to be shown to human users.  The returned string should
 * be in a format from which the GAME_MOVE can be recovered by applying
 * game_parse_move() to it.  The returned string is in malloc'ed storage,
 * which it is the responsibility of the caller to free when it is no
 * longer needed.
 *
 * @param move  The GAME_MOVE whose description is to be obtained.
 * @return  A string describing the specified GAME_MOVE.
 */
char *game_unparse_move(GAME_MOVE *move) {
	char *str = calloc(5, sizeof(char));
	str[0] = move->placement + 48;
	str[1] = '-';
	str[2] = '>';
	char c;
	if (move->piece == 1) {
		c = 'X';
	} else {
		c = 'O';
	}
	str[3] = c;
	return str;
}

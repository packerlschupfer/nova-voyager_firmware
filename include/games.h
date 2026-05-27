/**
 * @file games.h
 * @brief Shared constants for Nova Voyager Games firmware
 */

#ifndef GAMES_H
#define GAMES_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Display                                                                    */
/*===========================================================================*/

#define SCREEN_W    128
#define SCREEN_H     64

/*===========================================================================*/
/* Pong                                                                       */
/*===========================================================================*/

#define TICK_PONG_SLOW   40   /* ms/frame at game start */
#define TICK_PONG_MID    30   /* ms/frame after 5 paddle hits */
#define TICK_PONG_FAST   20   /* ms/frame after 10 paddle hits */
#define PONG_WIN_SCORE    5   /* first to this score wins */

/*===========================================================================*/
/* Snake                                                                      */
/*===========================================================================*/

#define SNAKE_CELL       4    /* pixels per grid cell (4×4) */
#define SNAKE_SCORE_H    8    /* pixel rows reserved for score display */
#define SNAKE_GAME_Y0    SNAKE_SCORE_H
#define SNAKE_GAME_H     (SCREEN_H - SNAKE_SCORE_H)   /* 56 */
#define SNAKE_COLS       (SCREEN_W / SNAKE_CELL)       /* 32 */
#define SNAKE_ROWS       (SNAKE_GAME_H / SNAKE_CELL)   /* 14 */
#define SNAKE_MAX_LEN    360
#define SNAKE_TICK_START 250  /* ms/tick at game start */
#define SNAKE_TICK_MIN    80  /* ms/tick floor */
#define SNAKE_TICK_STEP   10  /* ms reduction per food eaten */

#endif /* GAMES_H */

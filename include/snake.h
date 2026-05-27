/**
 * @file snake.h
 * @brief Snake game
 *
 * Encoder CW  → turn right
 * Encoder CCW → turn left
 * F1: restart after game over
 * F2: exit to game select menu
 */

#ifndef SNAKE_H
#define SNAKE_H

/**
 * @brief Run Snake until the player exits (F2).
 * Returns when F2 is pressed from the game-over screen.
 * LCD is left in graphics-mode-disabled state on return.
 */
void snake_run(void);

#endif /* SNAKE_H */

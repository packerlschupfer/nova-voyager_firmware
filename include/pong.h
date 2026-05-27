/**
 * @file pong.h
 * @brief Pong game
 *
 * P1 (left):  encoder controls paddle
 * P2 (right): ADC depth sensor controls paddle
 * F1: restart after game over
 * F2: exit to game select menu
 */

#ifndef PONG_H
#define PONG_H

/**
 * @brief Run Pong until the player exits (F2) or wins.
 * Returns when F2 is pressed or after displaying win screen.
 * LCD is left in graphics-mode-disabled state on return.
 */
void pong_run(void);

#endif /* PONG_H */

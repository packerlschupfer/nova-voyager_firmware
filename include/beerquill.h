/**
 * @file beerquill.h
 * @brief BeerQuill — pour the perfect pint
 *
 * Quill (plunge lever) = tap handle: pull to pour, depth = pour rate.
 * Encoder = glass tilt: tilt to suppress foam, straighten to build head.
 * F1: serve the pint.  F2: exit.
 */

#ifndef BEERQUILL_H
#define BEERQUILL_H

/**
 * @brief Run BeerQuill until the player exits (F2 from game-over screen).
 * LCD is left in graphics-mode-disabled state on return.
 */
void beerquill_run(void);

#endif /* BEERQUILL_H */

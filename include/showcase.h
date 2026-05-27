/**
 * @file showcase.h
 * @brief Graphics showcase — cycling ST7920 demo scenes
 *
 * A full-screen graphics demo (bouncing ball, sweep, expanding box, icon grid,
 * checkerboard) to show off the 128x64 display. Launched like a game (UI task
 * suspended). F2 exits; F3/F4 step scenes.
 */

#ifndef SHOWCASE_H
#define SHOWCASE_H

/** @brief Run the graphics showcase until F2. Leaves graphics mode disabled. */
void showcase_run(void);

#endif /* SHOWCASE_H */

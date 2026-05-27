/**
 * @file showcase.c
 * @brief Graphics showcase — cycling ST7920 demo scenes (128x64)
 *
 * Shows off the display for potential users: a sequence of animated scenes
 * rendered with the existing lcd_graphics primitives. Auto-advances every few
 * seconds; F3/F4 step scenes manually; F2 exits. Launched via game_launch()
 * so the UI task is suspended (no LCD contention), exactly like the games.
 */

#include "showcase.h"
#include "games.h"
#include "hw.h"
#include "lcd.h"
#include "lcd_graphics.h"
#include "game_font.h"
#include <stdint.h>
#include <stdbool.h>

#define SCENE_MS   4000     /* auto-advance interval */
#define TICK_MS      40     /* ~25 fps */
#define NUM_SCENES    5

/* Draw a right-aligned 3-digit number ending at x (reuses game_draw_digit) */
static void draw_num3(int16_t x, int16_t y, uint16_t v) {
    game_draw_digit(x, y, (uint8_t)(v % 10));
    if (v >= 10)  game_draw_digit(x - 4, y, (uint8_t)((v / 10) % 10));
    if (v >= 100) game_draw_digit(x - 8, y, (uint8_t)((v / 100) % 10));
}

/* Scene 0 — bouncing ball with a frame counter */
static void scene_ball(uint32_t f) {
    int16_t cx = (int16_t)(f * 3 % (2 * (SCREEN_W - 6)));
    if (cx >= SCREEN_W - 6) cx = 2 * (SCREEN_W - 6) - cx;
    int16_t cy = (int16_t)(f * 2 % (2 * (SCREEN_H - 6)));
    if (cy >= SCREEN_H - 6) cy = 2 * (SCREEN_H - 6) - cy;
    lcd_graphics_rect(0, 0, SCREEN_W - 1, SCREEN_H - 1, true);
    lcd_graphics_fill_rect(cx, cy, 6, 6, true);
}

/* Scene 1 — expanding/contracting concentric boxes */
static void scene_boxes(uint32_t f) {
    int16_t step = (int16_t)(f % 16);
    for (int16_t i = step; i < SCREEN_H / 2; i += 8) {
        lcd_graphics_rect(SCREEN_W / 2 - i, SCREEN_H / 2 - i, i * 2, i * 2, true);
    }
}

/* Scene 2 — horizontal sweep line */
static void scene_sweep(uint32_t f) {
    int16_t x = (int16_t)(f * 4 % SCREEN_W);
    for (int16_t y = 0; y < SCREEN_H; y++) lcd_graphics_pixel(x, y, true);
    for (int16_t y = 0; y < SCREEN_H; y += 4) {
        lcd_graphics_pixel(0, y, true);
        lcd_graphics_pixel(SCREEN_W - 1, y, true);
    }
}

/* Scene 3 — icon grid (filled + hollow tiles toggling) */
static void scene_grid(uint32_t f) {
    bool phase = (f / 8) & 1;
    for (int16_t gy = 0; gy < SCREEN_H; gy += 16) {
        for (int16_t gx = 0; gx < SCREEN_W; gx += 16) {
            bool fill = (((gx / 16) + (gy / 16)) & 1) == (phase ? 1 : 0);
            if (fill) lcd_graphics_fill_rect(gx + 3, gy + 3, 10, 10, true);
            else      lcd_graphics_rect(gx + 3, gy + 3, 10, 10, true);
        }
    }
}

/* Scene 4 — scrolling diagonal checkerboard */
static void scene_check(uint32_t f) {
    int16_t o = (int16_t)(f % 8);
    for (int16_t y = 0; y < SCREEN_H; y++)
        for (int16_t x = 0; x < SCREEN_W; x++)
            if ((((x + o) / 4) + (y / 4)) & 1) lcd_graphics_pixel(x, y, true);
}

void showcase_run(void) {
    lcd_graphics_mode(true);

    uint8_t scene = 0;
    uint32_t frame = 0;
    uint32_t last_tick = millis();
    uint32_t scene_start = millis();

    for (;;) {
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
            break;
        }
        if (btn_f3_pressed()) {
            while (btn_f3_pressed()) { (void)millis(); delay_ms(10); }
            scene = (scene + NUM_SCENES - 1) % NUM_SCENES;
            frame = 0; scene_start = millis();
        }
        if (btn_f4_pressed()) {
            while (btn_f4_pressed()) { (void)millis(); delay_ms(10); }
            scene = (scene + 1) % NUM_SCENES;
            frame = 0; scene_start = millis();
        }

        if ((millis() - last_tick) < TICK_MS) { delay_ms(1); continue; }
        last_tick = millis();

        /* Auto-advance */
        if ((millis() - scene_start) >= SCENE_MS) {
            scene = (scene + 1) % NUM_SCENES;
            frame = 0; scene_start = millis();
        }

        lcd_graphics_clear();
        switch (scene) {
            case 0: scene_ball(frame);   break;
            case 1: scene_boxes(frame);  break;
            case 2: scene_sweep(frame);  break;
            case 3: scene_grid(frame);   break;
            default: scene_check(frame); break;
        }
        /* Scene number top-left corner */
        draw_num3(6, 1, scene + 1);
        lcd_graphics_update();
        frame++;
    }

    lcd_graphics_mode(false);
}

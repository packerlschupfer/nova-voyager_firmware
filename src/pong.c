/**
 * @file pong.c
 * @brief Pong — 128×64 ST7920 graphics display
 *
 * Field:  128×64 pixels
 * Start screen selects 1-player (AI) or 2-player (quill) mode.
 * P1 left paddle  (x=2):   encoder relative movement
 * P2 right paddle (x=122): AI (1P) or quill ADC (2P)
 * Speed ramps up with rally length; F2 exits.
 * Ball: 3×3 px, integer velocity ±1
 * Score: 3×5 pixel-font digits at top-centre
 * Win: first to PONG_WIN_SCORE points
 */

#include "pong.h"
#include "games.h"
#include "hw.h"
#include "lcd.h"
#include "lcd_graphics.h"
#include "game_font.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Layout constants                                                           */
/*===========================================================================*/

#define P1_X        2
#define P2_X       122      /* left edge; right edge at 124; gap from 127 edge */
#define PAD_W       3
#define PAD_H      12
#define PAD_MAX_Y  (SCREEN_H - PAD_H)   /* 52 */

#define BALL_W      3
#define BALL_H      3

/* AI (single-player P2) — deliberately beatable */
#define AI_MAX_STEP   1     /* px/tick paddle moves (== ball vertical speed) */
#define AI_DEADZONE   2     /* tolerance band to avoid jitter */
#define AI_TRACK_X    (SCREEN_W * 2 / 3)  /* only track once ball enters P2's third */

/* Rally speed ramp: each paddle hit shaves TICK_PONG_DROP ms off the frame
 * time, from TICK_PONG_SLOW down to TICK_PONG_MIN. Resets when a point is
 * scored (hits → 0). Gives ~14 smooth speed levels instead of 3 steps. */
#define TICK_PONG_MIN   12  /* fastest (long-rally floor) */
#define TICK_PONG_DROP   2  /* ms shaved per hit */

/*===========================================================================*/
/* State                                                                      */
/*===========================================================================*/

typedef struct {
    int16_t ball_x, ball_y;
    int8_t  ball_vx, ball_vy;
    int16_t p1_y, p2_y;
    uint8_t score_p1, score_p2;
    uint8_t hits;           /* rally length → speed ramp */
} pong_t;

/*===========================================================================*/
/* Helpers                                                                    */
/*===========================================================================*/

static void reset_ball(pong_t *s) {
    s->ball_x  = SCREEN_W / 2 - 1;
    s->ball_y  = SCREEN_H / 2 - 1;
    s->ball_vx = 1;
    s->ball_vy = 1;
    s->hits    = 0;
}

/* Move P2 one step toward target_y, capped at AI_MAX_STEP, clamped to field */
static void p2_move_toward(pong_t *s, int16_t target_y) {
    int16_t pad_center = s->p2_y + PAD_H / 2;
    int16_t diff = target_y - pad_center;
    if (diff > AI_DEADZONE)        s->p2_y += (diff > AI_MAX_STEP) ? AI_MAX_STEP : diff;
    else if (diff < -AI_DEADZONE)  s->p2_y += (diff < -AI_MAX_STEP) ? -AI_MAX_STEP : diff;
    if (s->p2_y < 0)         s->p2_y = 0;
    if (s->p2_y > PAD_MAX_Y) s->p2_y = PAD_MAX_Y;
}

/* Start screen — returns true for 1-player (AI), false for 2-player (quill),
 * or aborts to caller via *exit if F2 held to quit. */
static bool pong_select_mode(bool *exit) {
    *exit = false;
    lcd_graphics_mode(false);
    lcd_cmd(0x38); lcd_cmd(0x0C); lcd_cmd(0x06);
    lcd_clear();
    lcd_print_at(0, 0, "      PONG      ");
    lcd_print_at(1, 0, "F1 = 1 PLAYER   ");
    lcd_print_at(2, 0, "F3 = 2 PLAYER   ");
    lcd_print_at(3, 0, "F2 = EXIT       ");
    for (;;) {
        // AUDIT FIX (MEDIUM, pong.c:94): wait screens used to spin without
        // millis() (which is the UI heartbeat feed) — anything >~2 s on a
        // mode-select or game-over screen tripped the watchdog. millis()
        // return is discarded; the side effect is what matters.
        (void)millis();
        if (btn_f1_pressed()) { while (btn_f1_pressed()) { (void)millis(); delay_ms(10); } return true; }
        if (btn_f3_pressed()) { while (btn_f3_pressed()) { (void)millis(); delay_ms(10); } return false; }
        if (btn_f2_pressed()) { while (btn_f2_pressed()) { (void)millis(); delay_ms(10); } *exit = true; return false; }
        delay_ms(10);
    }
}

static void draw_frame(const pong_t *s) {
    lcd_graphics_clear();

    /* Centre dashed line — 2px dash, 2px gap */
    for (int16_t y = 0; y < SCREEN_H; y += 4) {
        lcd_graphics_pixel(SCREEN_W / 2, y,     true);
        lcd_graphics_pixel(SCREEN_W / 2, y + 1, true);
    }

    /* Paddles */
    lcd_graphics_fill_rect(P1_X, s->p1_y, PAD_W, PAD_H, true);
    lcd_graphics_fill_rect(P2_X, s->p2_y, PAD_W, PAD_H, true);

    /* Ball */
    lcd_graphics_fill_rect(s->ball_x, s->ball_y, BALL_W, BALL_H, true);

    /* Scores: P1 left of centre, P2 right of centre (top row) */
    game_draw_digit((int16_t)(SCREEN_W / 2 - 9), 1, s->score_p1);
    game_draw_digit((int16_t)(SCREEN_W / 2 + 5), 1, s->score_p2);

    lcd_graphics_update();
}

/*===========================================================================*/
/* pong_run                                                                   */
/*===========================================================================*/

void pong_run(void) {
    /* Start screen: pick 1-player (AI) or 2-player (quill) */
    bool want_exit;
    bool ai_enabled = pong_select_mode(&want_exit);
    if (want_exit) {
        lcd_graphics_mode(false);
        return;
    }

    pong_t s;
    memset(&s, 0, sizeof(s));
    s.p1_y = (SCREEN_H - PAD_H) / 2;
    s.p2_y = (SCREEN_H - PAD_H) / 2;
    reset_ball(&s);

    lcd_graphics_mode(true);
    draw_frame(&s);

    uint32_t last_tick = millis();
    uint16_t tick_ms   = TICK_PONG_SLOW;

    int16_t enc_acc = 0;

    for (;;) {

        /* --- Exit --- */
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }   /* wait for release */
            break;
        }

        /* --- Poll encoder continuously between frames --- */
        enc_acc += encoder_read_delta();

        /* --- Wait for next tick --- */
        if ((millis() - last_tick) < tick_ms) { delay_ms(1); continue; }
        last_tick = millis();

        /* --- P1 paddle: apply accumulated encoder movement --- */
        s.p1_y += enc_acc * 2;
        enc_acc = 0;
        if (s.p1_y < 0)          s.p1_y = 0;
        if (s.p1_y > PAD_MAX_Y)  s.p1_y = PAD_MAX_Y;

        /* --- P2 paddle: AI (single-player) or quill ADC (two-player) --- */
        if (ai_enabled) {
            /* Track the ball only once it enters P2's third and is approaching;
             * otherwise drift to centre. The capped step + late tracking make
             * angled shots and corner returns beatable. */
            int16_t target;
            if (s.ball_vx > 0 && s.ball_x >= AI_TRACK_X) {
                target = s.ball_y + BALL_H / 2;
            } else {
                target = SCREEN_H / 2;
            }
            p2_move_toward(&s, target);
        } else {
            uint16_t adc = adc_read_raw();
            int16_t clamped = (int16_t)adc - 500;
            if (clamped < 0)   clamped = 0;
            if (clamped > 200) clamped = 200;
            s.p2_y = (int16_t)((uint32_t)clamped * PAD_MAX_Y / 200);
        }

        /* --- Ball movement --- */
        s.ball_x += s.ball_vx;
        s.ball_y += s.ball_vy;

        /* Top/bottom wall bounce */
        if (s.ball_y <= 0) {
            s.ball_y  = 0;
            s.ball_vy = 1;
            buzz(1200, 8);
        }
        if (s.ball_y >= SCREEN_H - BALL_H) {
            s.ball_y  = SCREEN_H - BALL_H;
            s.ball_vy = -1;
            buzz(1200, 8);
        }

        /* P1 paddle collision (moving left) */
        if (s.ball_vx < 0 &&
            s.ball_x <= P1_X + PAD_W &&
            s.ball_x >= P1_X - 1 &&
            s.ball_y + BALL_H > s.p1_y &&
            s.ball_y < s.p1_y + PAD_H)
        {
            s.ball_x  = P1_X + PAD_W;
            s.ball_vx = 1;
            s.hits++;
            /* Angle: hit top-third → up, bottom-third → down, middle → keep */
            int16_t rel = s.ball_y - s.p1_y;
            if (rel < PAD_H / 3)       s.ball_vy = -1;
            else if (rel > 2 * PAD_H / 3) s.ball_vy = 1;
            buzz(900, 15);
        }

        /* P2 paddle collision (moving right) */
        if (s.ball_vx > 0 &&
            s.ball_x + BALL_W >= P2_X &&
            s.ball_x <= P2_X + PAD_W &&
            s.ball_y + BALL_H > s.p2_y &&
            s.ball_y < s.p2_y + PAD_H)
        {
            s.ball_x  = P2_X - BALL_W;
            s.ball_vx = -1;
            s.hits++;
            int16_t rel = s.ball_y - s.p2_y;
            if (rel < PAD_H / 3)       s.ball_vy = -1;
            else if (rel > 2 * PAD_H / 3) s.ball_vy = 1;
            buzz(700, 15);
        }

        /* Speed ramp: faster the longer the rally; resets when a point scores */
        /* AUDIT FIX (LOW, pong.c:248): computed in uint16_t, so once
         * hits * TICK_PONG_DROP exceeded TICK_PONG_SLOW the subtraction wrapped
         * to ~65534 and the clamp below — which only raises values — could not
         * catch it. A 21-hit rally froze the game for 65 seconds. Do the
         * arithmetic signed, then clamp. */
        int32_t next_tick = (int32_t)TICK_PONG_SLOW -
                            (int32_t)s.hits * TICK_PONG_DROP;
        if (next_tick < TICK_PONG_MIN) next_tick = TICK_PONG_MIN;
        tick_ms = (uint16_t)next_tick;

        /* Miss: ball leaves left edge → P2 scores */
        if (s.ball_x < 0) {
            s.score_p2++;
            buzz(400, 100);
            delay_ms(50);
            buzz(300, 100);
            reset_ball(&s);
            draw_frame(&s);
        }

        /* Miss: ball leaves right edge → P1 scores */
        if (s.ball_x >= SCREEN_W) {
            s.score_p1++;
            buzz(400, 100);
            delay_ms(50);
            buzz(300, 100);
            reset_ball(&s);
            draw_frame(&s);
        }

        /* Win check */
        if (s.score_p1 >= PONG_WIN_SCORE || s.score_p2 >= PONG_WIN_SCORE) {
            /* Victory fanfare */
            buzz(523, 120);
            delay_ms(60);
            buzz(659, 120);
            delay_ms(60);
            buzz(784, 240);

            /* Show winner on character LCD */
            lcd_graphics_mode(false);
            lcd_cmd(0x38); lcd_cmd(0x0C); lcd_cmd(0x06);
            lcd_clear();
            /* REVIEW FIX: 16 characters starting at word 1 is character 2, so
             * the last two ran past the row and onto row 2. Start at word 0
             * and drop the padding spaces that were doing the centring. */
            if (s.score_p1 >= PONG_WIN_SCORE) {
                lcd_print_at(0, 0, " PLAYER 1 WINS!");
            } else {
                lcd_print_at(0, 0, " PLAYER 2 WINS!");
            }
            lcd_print_at(1, 0, "F1=PLAY AGAIN   ");
            lcd_print_at(2, 0, "F2=MENU         ");

            /* Wait for F1 (replay) or F2 (exit) */
            for (;;) {
                (void)millis();  // heartbeat feed (see pong_select_mode)
                if (btn_f1_pressed()) {
                    while (btn_f1_pressed()) { (void)millis(); delay_ms(10); }
                    /* Restart: re-enter graphics and reset state */
                    memset(&s, 0, sizeof(s));
                    s.p1_y = (SCREEN_H - PAD_H) / 2;
                    s.p2_y = (SCREEN_H - PAD_H) / 2;
                    reset_ball(&s);
                    lcd_graphics_mode(true);
                    draw_frame(&s);
                    tick_ms = TICK_PONG_SLOW;
                    break;
                }
                if (btn_f2_pressed()) {
                    while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
                    lcd_graphics_mode(false);
                    return;
                }
                delay_ms(10);
            }
            continue;
        }

        draw_frame(&s);
    }

    lcd_graphics_mode(false);
}

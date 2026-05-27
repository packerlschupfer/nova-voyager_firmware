/**
 * @file beerquill.c
 * @brief BeerQuill — pour the perfect pint (128×64 ST7920)
 *
 * Pull the quill (plunge lever) to open the tap; pull depth sets pour rate.
 * Beer fills the glass from the bottom; pouring also builds a foam head.
 * Pouring with the glass UPRIGHT makes lots of foam; TILT the glass with the
 * encoder to pour down the side and suppress foam, then straighten up to build
 * the head. Foam slowly settles (and a little turns to beer, so don't dawdle).
 *
 * Goal: get the beer to the dashed target line with a foam head inside the
 * band marked on the right wall, then press F1 to serve. Overflow = spill,
 * and an impatient customer (patience bar runs out) both cost a life.
 */

#include "beerquill.h"
#include "games.h"
#include "hw.h"
#include "lcd.h"
#include "lcd_graphics.h"
#include "game_font.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Glass geometry                                                             */
/*===========================================================================*/

#define G_X0     48      /* glass left wall  */
#define G_X1     84      /* glass right wall */
#define G_TOP    10      /* glass rim (y)    */
#define G_BOT    62      /* glass base (y)   */
#define FILL_X0  (G_X0 + 2)
#define FILL_X1  (G_X1 - 2)
#define FILL_W   (FILL_X1 - FILL_X0)
#define FILL_BOT (G_BOT - 2)              /* y of liquid floor */
#define FILL_TOP (G_TOP + 2)              /* y of rim (overflow) */
#define H_PX     (FILL_BOT - FILL_TOP)    /* interior height in px (50) */

#define FX        16                      /* sub-pixel scale */
#define H_FX     (H_PX * FX)

/*===========================================================================*/
/* Tuning                                                                     */
/*===========================================================================*/

#define TILT_MAX     16
#define POUR_DIV      6      /* quill-pull → pour_fx divisor (bigger = slower) */
/* Quill ADC: weak return spring near the rest position gives poor control at
 * the very bottom, so the usable, controllable travel sits around 200..400.
 * REST = tap-closed point; SPAN = pull depth that reaches full pour rate. */
#define ADC_REST    400      /* quill resting ADC (tap closed) */
#define ADC_SPAN    400      /* pull beyond REST for full pour (400 → 800) */
#define LIVES_START   3
#define PAT_START   600      /* patience ticks (~24 s at 40 ms) */

/*===========================================================================*/
/* State                                                                      */
/*===========================================================================*/

typedef struct {
    int32_t beer_fx;     /* beer column height, sub-pixels */
    int32_t foam_fx;     /* foam head height, sub-pixels   */
    int16_t tilt;        /* 0 = upright .. TILT_MAX        */
    int16_t target_px;   /* target beer line (px from floor) */
    int16_t tol;         /* beer-line tolerance (px)       */
    int16_t head_min;    /* head band (px)                 */
    int16_t head_max;
    uint16_t patience;   /* ticks remaining for this customer */
    uint16_t pat_max;
    uint16_t score;
    uint8_t  round;
    uint8_t  lives;
} bq_t;

/*===========================================================================*/
/* Helpers                                                                    */
/*===========================================================================*/

/* Draw a right-aligned decimal number (up to 3 digits) ending at x */
static void draw_num(int16_t x, int16_t y, uint16_t v) {
    game_draw_digit(x, y, (uint8_t)(v % 10));
    if (v >= 10)  game_draw_digit(x - 4, y, (uint8_t)((v / 10) % 10));
    if (v >= 100) game_draw_digit(x - 8, y, (uint8_t)((v / 100) % 10));
}

/* Configure a new customer's target based on the round number */
static void next_customer(bq_t *s) {
    s->beer_fx = 0;
    s->foam_fx = 0;
    s->tilt    = 0;
    uint8_t r  = s->round;
    s->target_px = 28 + (r % 4) * 5;                 /* 28,33,38,43 cycling */
    s->tol       = (r < 6) ? (5 - r / 2) : 2;        /* 5 → 2 px */
    if (s->tol < 2) s->tol = 2;
    s->head_min  = 5;
    s->head_max  = (r < 8) ? (12 - r) : 4;           /* 12 → 4 px band shrinks */
    if (s->head_max < s->head_min + 2) s->head_max = s->head_min + 2;
    s->pat_max   = (r < 10) ? (PAT_START - r * 30) : 300;
    s->patience  = s->pat_max;
}

/*===========================================================================*/
/* Rendering                                                                  */
/*===========================================================================*/

static void draw_frame(const bq_t *s) {
    lcd_graphics_clear();

    /* --- HUD: score (top-left), lives (top-right) --- */
    draw_num(20, 1, s->score);
    for (int i = 0; i < s->lives; i++)
        lcd_graphics_fill_rect(2 + i * 5, 1, 3, 5, true);

    /* Round number bottom-left */
    draw_num(8, G_BOT - 5, s->round + 1);

    /* Patience bar (left side, vertical, drains downward) */
    {
        int16_t full = H_PX;
        int16_t lvl  = (int16_t)((uint32_t)s->patience * full / s->pat_max);
        lcd_graphics_rect(2, FILL_TOP, 4, H_PX, true);
        if (lvl > 0)
            lcd_graphics_fill_rect(3, FILL_BOT - lvl, 2, lvl, true);
    }

    /* --- Glass outline --- */
    lcd_graphics_rect(G_X0, G_TOP, G_X1 - G_X0, G_BOT - G_TOP, true);

    /* --- Beer + foam fill --- */
    int16_t beer_px = (int16_t)(s->beer_fx / FX);
    int16_t foam_px = (int16_t)(s->foam_fx / FX);
    if (beer_px > H_PX) beer_px = H_PX;
    if (beer_px + foam_px > H_PX) foam_px = H_PX - beer_px;

    /* Beer: solid block from floor up */
    if (beer_px > 0)
        lcd_graphics_fill_rect(FILL_X0, FILL_BOT - beer_px, FILL_W, beer_px, true);

    /* Foam: dotted texture above beer */
    int16_t foam_top = FILL_BOT - beer_px - foam_px;
    for (int16_t y = foam_top; y < FILL_BOT - beer_px; y++)
        for (int16_t x = FILL_X0; x < FILL_X1; x++)
            if (((x + y) & 1) == 0) lcd_graphics_pixel(x, y, true);

    /* --- Target beer line (dashed across glass) --- */
    int16_t line_y = FILL_BOT - s->target_px;
    for (int16_t x = G_X0; x <= G_X1; x += 3)
        lcd_graphics_pixel(x, line_y, true);

    /* --- Head band ticks on the right wall (absolute: above the line) --- */
    int16_t band_lo = line_y - s->head_min;   /* nearer the line */
    int16_t band_hi = line_y - s->head_max;   /* higher up */
    for (int16_t x = G_X1 + 1; x <= G_X1 + 4; x++) {
        lcd_graphics_pixel(x, band_lo, true);
        lcd_graphics_pixel(x, band_hi, true);
    }

    /* --- Tilt gauge (right side, needle slides with tilt) --- */
    {
        int16_t gx = 100;
        lcd_graphics_rect(gx, FILL_TOP, 24, 6, true);
        int16_t nx = gx + 1 + (int16_t)((uint32_t)s->tilt * 20 / TILT_MAX);
        lcd_graphics_fill_rect(nx, FILL_TOP + 1, 3, 4, true);
    }

    lcd_graphics_update();
}

/*===========================================================================*/
/* Char-mode result / game-over screens                                       */
/*===========================================================================*/

static void char_mode(void) {
    lcd_graphics_mode(false);
    lcd_cmd(0x38); lcd_cmd(0x0C); lcd_cmd(0x06);
    lcd_clear();
}

/*===========================================================================*/
/* beerquill_run                                                              */
/*===========================================================================*/

void beerquill_run(void) {
restart: ;
    bq_t s;
    memset(&s, 0, sizeof(s));
    s.score = 0;
    s.round = 0;
    s.lives = LIVES_START;
    next_customer(&s);

    lcd_graphics_mode(true);
    draw_frame(&s);

    uint32_t last_tick = millis();
    const uint16_t tick_ms = 40;

    for (;;) {
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
            lcd_graphics_mode(false);
            return;
        }

        /* Tilt follows the encoder continuously */
        s.tilt += encoder_read_delta();
        if (s.tilt < 0)         s.tilt = 0;
        if (s.tilt > TILT_MAX)  s.tilt = TILT_MAX;

        if ((millis() - last_tick) < tick_ms) { delay_ms(1); continue; }
        last_tick = millis();

        /* --- Pour: quill pull → beer + foam --- */
        int16_t pull = (int16_t)adc_read_raw() - ADC_REST;
        if (pull < 0)        pull = 0;
        if (pull > ADC_SPAN) pull = ADC_SPAN;
        int32_t pour_fx = pull / POUR_DIV;
        if (pour_fx > 0) {
            s.beer_fx += pour_fx;
            /* Upright pour foams hard; tilt suppresses it */
            int32_t coeff = 100 - (int32_t)s.tilt * 80 / TILT_MAX;  /* 100..20 % */
            s.foam_fx += pour_fx * coeff / 100;
        }

        /* --- Foam settles; a little converts back to beer --- */
        if (s.foam_fx > 0) {
            int32_t settle = s.foam_fx / 32;
            if (settle < 6) settle = 6;
            s.foam_fx -= settle;
            if (s.foam_fx < 0) s.foam_fx = 0;
            s.beer_fx += settle / 4;
        }

        /* --- Overflow → spill --- */
        if (s.beer_fx + s.foam_fx >= H_FX) {
            buzz(200, 250);
            char_mode();
            lcd_print_at(1, 0, "   SPILL!  X    ");
            delay_ms(900);
            if (--s.lives == 0) goto game_over;
            next_customer(&s);
            lcd_graphics_mode(true);
            draw_frame(&s);
            continue;
        }

        /* --- Patience runs out → customer walks --- */
        if (s.patience == 0) {
            buzz(300, 200);
            char_mode();
            lcd_print_at(1, 0, " TOO SLOW! walk ");
            delay_ms(900);
            if (--s.lives == 0) goto game_over;
            next_customer(&s);
            lcd_graphics_mode(true);
            draw_frame(&s);
            continue;
        }
        s.patience--;

        /* --- Serve (F1): grade the pour --- */
        if (btn_f1_pressed()) {
            while (btn_f1_pressed()) { (void)millis(); delay_ms(10); }
            int16_t beer_px = (int16_t)(s.beer_fx / FX);
            int16_t foam_px = (int16_t)(s.foam_fx / FX);
            int16_t err = beer_px - s.target_px;
            if (err < 0) err = -err;
            bool head_ok = (foam_px >= s.head_min && foam_px <= s.head_max);

            const char *msg;
            uint16_t pts;
            if (err <= s.tol && head_ok) {
                pts = 100 + s.patience / 4;    /* speed bonus */
                msg = "  PERFECT PINT! ";
                buzz(523, 100); delay_ms(60); buzz(659, 100); delay_ms(60); buzz(784, 160);
            } else if (err <= s.tol * 2 && foam_px >= 2) {
                pts = 40;
                msg = "   NOT BAD...   ";
                buzz(659, 120);
            } else {
                pts = 10;
                msg = "  SLOPPY POUR   ";
                buzz(400, 120);
            }
            s.score += pts;
            s.round++;

            char_mode();
            lcd_print_at(1, 0, msg);
            lcd_print_at(2, 4, "+");
            { char b[6]; int i = 0; uint16_t v = pts;
              char tmp[6]; int n = 0;
              do { tmp[n++] = '0' + v % 10; v /= 10; } while (v && n < 5);
              while (n > 0) b[i++] = tmp[--n];
              b[i] = '\0';
              lcd_print_at(2, 5, b); }
            delay_ms(900);

            next_customer(&s);
            lcd_graphics_mode(true);
            draw_frame(&s);
            continue;
        }

        draw_frame(&s);
    }

game_over:
    buzz(500, 80); delay_ms(40); buzz(350, 80); delay_ms(40); buzz(200, 200);
    char_mode();
    lcd_print_at(0, 0, "   LAST CALL!   ");
    {
        char line[17];
        const char *p = "Score:";
        int i = 0;
        while (p[i]) { line[i] = p[i]; i++; }
        uint16_t v = s.score;
        char tmp[6]; int n = 0;
        do { tmp[n++] = '0' + v % 10; v /= 10; } while (v && n < 5);
        while (n > 0) line[i++] = tmp[--n];
        while (i < 16) line[i++] = ' ';
        line[16] = '\0';
        lcd_print_at(1, 0, line);
    }
    lcd_print_at(2, 0, "F1=PLAY AGAIN   ");
    lcd_print_at(3, 0, "F2=MENU         ");

    for (;;) {
        (void)millis();  // AUDIT FIX (MEDIUM, pong.c:94): UI heartbeat feed
        if (btn_f1_pressed()) {
            while (btn_f1_pressed()) { (void)millis(); delay_ms(10); }
            goto restart;
        }
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
            lcd_graphics_mode(false);
            return;
        }
        delay_ms(10);
    }
}

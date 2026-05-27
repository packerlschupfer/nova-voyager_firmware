/**
 * @file penguin.c
 * @brief Penguin Run — side-scrolling obstacle dodger
 *
 * A penguin slides across ice, dodging obstacles that scroll from right.
 * Quill (ADC) controls vertical position. Pedal = jump/boost.
 * Score increases as obstacles pass. Speed ramps up over time.
 */

#include "penguin.h"
#include "games.h"
#include "hw.h"
#include "lcd.h"
#include "lcd_graphics.h"
#include "game_font.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define PENGUIN_X       12
#define PENGUIN_W        8
#define PENGUIN_HT      10
#define GROUND_Y        58
#define SKY_Y            0
#define PLAY_H          (GROUND_Y - SKY_Y)

#define OBS_W            6
#define GAP_H_START     26
#define GAP_H_MIN       18
#define OBS_SPACING     36
#define MAX_OBS          4

#define TICK_START       30
#define TICK_MIN         12
#define TICK_SPEEDUP    150

#define ADC_MIN         500
#define ADC_MAX         700

/* 8×10 Tux sprite — two frames for waddle animation
 * White belly, black body, eyes in white face, beak, feet */
static const uint8_t s_penguin[2][10] = {
    { /* frame 0 — wings out */
        0x3C,  /*   ####   head top        */
        0x7E,  /*  ######  head            */
        0x5A,  /*  #.##.#  eyes in face    */
        0x24,  /*   #  #   beak            */
        0x7E,  /*  ######  shoulders       */
        0xDB,  /* ##.##.## wings out+belly */
        0xC3,  /* ##....## belly wide      */
        0x66,  /*  ##  ##  body            */
        0x3C,  /*   ####   body bottom     */
        0x24,  /*   #  #   feet            */
    },
    { /* frame 1 — wings in */
        0x3C,  /*   ####   head top        */
        0x7E,  /*  ######  head            */
        0x5A,  /*  #.##.#  eyes in face    */
        0x24,  /*   #  #   beak            */
        0x7E,  /*  ######  shoulders       */
        0x66,  /*  ##  ##  body+belly      */
        0x42,  /*  #    #  belly wide      */
        0x66,  /*  ##  ##  body            */
        0x3C,  /*   ####   body bottom     */
        0x24,  /*   #  #   feet            */
    },
};

typedef struct {
    int16_t x;
    int16_t gap_y;
    int16_t gap_h;
    bool    active;
    bool    scored;
} obstacle_t;

#define JUMP_FORCE     -6
#define JUMP_DURATION  12
#define JUMP_COOLDOWN  20

typedef struct {
    int16_t     penguin_y;
    int16_t     base_y;
    int16_t     jump_vel;
    uint8_t     jump_timer;
    uint8_t     jump_cool;
    uint16_t    score;
    uint16_t    hi_score;
    uint8_t     frame;
    uint32_t    tick_ms;
    uint32_t    dist;
    obstacle_t  obs[MAX_OBS];
    uint8_t     next_obs;
} game_t;

static uint32_t s_rng;

static int16_t current_gap_h(uint16_t score) {
    int16_t gap = GAP_H_START - (int16_t)(score / 3);
    if (gap < GAP_H_MIN) gap = GAP_H_MIN;
    return gap;
}

static int16_t rand_gap_y(uint16_t score) {
    s_rng = s_rng * 1664525UL + 1013904223UL;
    int16_t gap_h = current_gap_h(score);
    int16_t range = PLAY_H - gap_h - 4;
    if (range < 4) range = 4;
    return (int16_t)((s_rng >> 16) % (uint16_t)range) + 2;
}

static void draw_score(int16_t x, int16_t y, uint16_t val) {
    if (val > 999) val = 999;
    if (val >= 100) { game_draw_digit(x, y, (uint8_t)(val / 100)); x += 4; }
    if (val >= 10)  { game_draw_digit(x, y, (uint8_t)((val / 10) % 10)); x += 4; }
    game_draw_digit(x, y, (uint8_t)(val % 10));
}

static void draw_penguin(const game_t *g) {
    const uint8_t *spr = s_penguin[g->frame & 1];
    for (int r = 0; r < PENGUIN_HT; r++) {
        for (int c = 0; c < 8; c++) {
            bool px = (spr[r] >> (7 - c)) & 1;
            lcd_graphics_pixel(PENGUIN_X + c, g->penguin_y + r, px);
        }
    }
}

static void draw_obstacle(const obstacle_t *o) {
    if (!o->active) return;
    /* Top pillar */
    lcd_graphics_fill_rect(o->x, 0, OBS_W, o->gap_y, true);
    /* Bottom pillar */
    int16_t bot_y = o->gap_y + o->gap_h;
    lcd_graphics_fill_rect(o->x, bot_y, OBS_W, GROUND_Y - bot_y, true);
    /* Pillar caps (1px wider) */
    lcd_graphics_fill_rect(o->x - 1, o->gap_y - 2, OBS_W + 2, 2, true);
    lcd_graphics_fill_rect(o->x - 1, bot_y, OBS_W + 2, 2, true);
}

static void draw_ground(void) {
    for (int16_t x = 0; x < SCREEN_W; x++)
        lcd_graphics_pixel(x, GROUND_Y, true);
}

static void render(const game_t *g) {
    lcd_graphics_clear();
    draw_ground();

    for (uint8_t i = 0; i < MAX_OBS; i++)
        draw_obstacle(&g->obs[i]);

    draw_penguin(g);

    /* Score top-right */
    draw_score(SCREEN_W - 14, 1, g->score);
    /* Hi-score top-left */
    draw_score(1, 1, g->hi_score);

    /* Jump cooldown bar (bottom-left, 1px tall) */
    if (g->jump_cool > 0) {
        int16_t bar_w = (int16_t)g->jump_cool * 20 / JUMP_COOLDOWN;
        lcd_graphics_fill_rect(1, GROUND_Y + 2, bar_w, 2, true);
    } else if (g->jump_timer == 0) {
        lcd_graphics_fill_rect(1, GROUND_Y + 2, 20, 2, true);
    }

    lcd_graphics_update();
}

static bool check_collision(const game_t *g) {
    for (uint8_t i = 0; i < MAX_OBS; i++) {
        const obstacle_t *o = &g->obs[i];
        if (!o->active) continue;
        /* Check horizontal overlap */
        if (PENGUIN_X + PENGUIN_W <= o->x || PENGUIN_X >= o->x + OBS_W)
            continue;
        /* Check if penguin is in the gap */
        if (g->penguin_y < o->gap_y || g->penguin_y + PENGUIN_HT > o->gap_y + o->gap_h)
            return true;
    }
    /* Floor/ceiling */
    if (g->penguin_y + PENGUIN_HT > GROUND_Y || g->penguin_y < 0)
        return true;
    return false;
}

static int16_t read_quill_y(void) {
    uint16_t adc = adc_read_raw();
    int16_t clamped = (int16_t)adc - ADC_MIN;
    if (clamped < 0)   clamped = 0;
    int16_t range = ADC_MAX - ADC_MIN;
    if (clamped > range) clamped = range;
    int16_t max_y = GROUND_Y - PENGUIN_HT;
    return (int16_t)((uint32_t)clamped * (uint16_t)max_y / (uint16_t)range);
}

static void show_game_over(uint16_t score, uint16_t hi) {
    lcd_graphics_mode(false);
    lcd_cmd(0x38); lcd_cmd(0x0C); lcd_cmd(0x06);
    lcd_clear();

    char line[17];
    const char *prefix = "Score:";
    int i = 0;
    while (prefix[i]) { line[i] = prefix[i]; i++; }
    line[i++] = (char)('0' + (score / 100) % 10);
    line[i++] = (char)('0' + (score / 10) % 10);
    line[i++] = (char)('0' + score % 10);
    /* REVIEW FIX: "Score:" + 3 + 2 spaces + "Hi:" + 3 is 17 characters on a
     * 16-column row, so line[16] = '\0' below landed on the hi-score's units
     * digit and every high score displayed as two digits. One space fits. */
    line[i++] = ' ';
    line[i++] = 'H'; line[i++] = 'i'; line[i++] = ':';
    line[i++] = (char)('0' + (hi / 100) % 10);
    line[i++] = (char)('0' + (hi / 10) % 10);
    line[i++] = (char)('0' + hi % 10);
    while (i < 16) line[i++] = ' ';
    line[16] = '\0';

    lcd_print_at(0, 0, "   GAME  OVER   ");
    lcd_print_at(1, 0, line);
    lcd_print_at(2, 0, "F1=RESTART      ");
    lcd_print_at(3, 0, "F2=MENU         ");
}

void penguin_run(void) {
    uint16_t hi_score = 0;

restart: ;
    s_rng = millis() ^ 54321;
    game_t g;
    memset(&g, 0, sizeof(g));
    g.penguin_y = GROUND_Y / 2;
    g.tick_ms   = TICK_START;

    lcd_graphics_mode(true);
    render(&g);

    uint32_t last_tick = millis();
    uint16_t spawn_dist = OBS_SPACING;

    for (;;) {
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
            goto done;
        }

        if ((millis() - last_tick) < g.tick_ms) { delay_ms(1); continue; }
        last_tick = millis();

        /* Read quill position */
        g.base_y = read_quill_y();

        /* Pedal jump boost */
        if (pedal_pressed() && g.jump_cool == 0 && g.jump_timer == 0) {
            g.jump_timer = JUMP_DURATION;
            g.jump_vel = JUMP_FORCE;
            buzz(1500, 10);
        }
        if (g.jump_timer > 0) {
            g.penguin_y = g.base_y + g.jump_vel;
            g.jump_vel += 1;
            g.jump_timer--;
            if (g.jump_timer == 0) g.jump_cool = JUMP_COOLDOWN;
        } else {
            g.penguin_y = g.base_y;
        }
        if (g.jump_cool > 0) g.jump_cool--;

        /* Clamp to play area */
        if (g.penguin_y < 0) g.penguin_y = 0;
        int16_t max_y = GROUND_Y - PENGUIN_HT;
        if (g.penguin_y > max_y) g.penguin_y = max_y;

        /* Animate penguin */
        g.dist++;
        if ((g.dist & 3) == 0) g.frame++;

        /* Spawn obstacles */
        spawn_dist--;
        if (spawn_dist == 0) {
            obstacle_t *o = &g.obs[g.next_obs];
            o->x      = SCREEN_W;
            o->gap_h  = current_gap_h(g.score);
            o->gap_y  = rand_gap_y(g.score);
            o->active = true;
            o->scored = false;
            g.next_obs = (g.next_obs + 1) % MAX_OBS;
            spawn_dist = OBS_SPACING;
        }

        /* Move obstacles left */
        for (uint8_t i = 0; i < MAX_OBS; i++) {
            obstacle_t *o = &g.obs[i];
            if (!o->active) continue;
            o->x -= 2;
            if (o->x + OBS_W < 0) {
                o->active = false;
                continue;
            }
            /* Score when obstacle passes penguin */
            if (!o->scored && o->x + OBS_W < PENGUIN_X) {
                o->scored = true;
                g.score++;
                if (g.score > hi_score) hi_score = g.score;
                if (g.score % 5 == 0)
                    buzz(1600, 30);
                else
                    buzz(1200, 15);
            }
        }

        /* Speed up */
        if (g.dist % TICK_SPEEDUP == 0 && g.tick_ms > TICK_MIN)
            g.tick_ms -= 2;

        /* Collision */
        if (check_collision(&g)) {
            buzz(600, 80);
            delay_ms(30);
            buzz(400, 80);
            delay_ms(30);
            buzz(200, 200);
            show_game_over(g.score, hi_score);

            for (;;) {
                (void)millis();  // AUDIT FIX (MEDIUM, pong.c:94): UI heartbeat feed
                if (btn_f1_pressed()) {
                    while (btn_f1_pressed()) { (void)millis(); delay_ms(10); }
                    lcd_graphics_mode(true);
                    goto restart;
                }
                if (btn_f2_pressed()) {
                    while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
                    goto done;
                }
                delay_ms(10);
            }
        }

        render(&g);
    }

done:
    lcd_graphics_mode(false);
}

/**
 * @file snake.c
 * @brief Snake — 128×64 ST7920 graphics display
 *
 * Layout:
 *   Rows 0–7:  score area ("S:xx H:xx" in 3×5 pixel font)
 *   Row  7:    1px separator line
 *   Rows 8–63: game area (56px tall)
 *
 * Grid: SNAKE_COLS=32, SNAKE_ROWS=14  (4px cells within 128×56 game area)
 * Border: 1px rect drawn at game-area boundary (inside the 8..63 y range).
 * Play field: cells (1..30, 1..12) → 30×12 interior.
 *
 * Snake storage: ring buffer, head at front.
 *   body[head_idx] is always the head.
 *   element i from head = body[(head_idx - i + MAX) % MAX]
 *
 * Controls:
 *   Encoder CW  → turn right
 *   Encoder CCW → turn left
 *   F1: restart (on game-over screen)
 *   F2: exit to menu
 */

#include "snake.h"
#include "games.h"
#include "hw.h"
#include "lcd.h"
#include "lcd_graphics.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Layout                                                                     */
/*===========================================================================*/

/* Game area pixel bounds (exclusive) */
#define GA_X0   0
#define GA_Y0   SNAKE_GAME_Y0          /* 8 */
#define GA_X1   (SCREEN_W - 1)         /* 127 */
#define GA_Y1   (SCREEN_H - 1)         /* 63 */

/* Play field: border at grid col 0, SNAKE_COLS-1, row 0, SNAKE_ROWS-1.
 * Interior cells: col 1..SNAKE_COLS-2, row 1..SNAKE_ROWS-2 */
#define C       SNAKE_CELL             /* 4 */

/* Convert grid col/row to pixel coords in game area */
#define CELL_PX(col)  (GA_X0 + (col) * C)
#define CELL_PY(row)  (GA_Y0 + (row) * C)

/*===========================================================================*/
/* 3×5 pixel font — digits + letters needed for score line                   */
/*===========================================================================*/

/* Indices: 0–9 = digits, 10='S', 11='H', 12=':', 13=' ' */
static const uint8_t s_font[][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7},   /*  0 */
    {0x2, 0x6, 0x2, 0x2, 0x7},   /*  1 */
    {0x7, 0x1, 0x7, 0x4, 0x7},   /*  2 */
    {0x7, 0x1, 0x7, 0x1, 0x7},   /*  3 */
    {0x5, 0x5, 0x7, 0x1, 0x1},   /*  4 */
    {0x7, 0x4, 0x7, 0x1, 0x7},   /*  5 */
    {0x7, 0x4, 0x7, 0x5, 0x7},   /*  6 */
    {0x7, 0x1, 0x2, 0x2, 0x2},   /*  7 */
    {0x7, 0x5, 0x7, 0x5, 0x7},   /*  8 */
    {0x7, 0x5, 0x7, 0x1, 0x7},   /*  9 */
    {0x7, 0x4, 0x7, 0x1, 0x7},   /* 10 'S' */
    {0x5, 0x5, 0x7, 0x5, 0x5},   /* 11 'H' */
    {0x0, 0x2, 0x0, 0x2, 0x0},   /* 12 ':' */
    {0x0, 0x0, 0x0, 0x0, 0x0},   /* 13 ' ' */
};

/* Draw one glyph at pixel coords (x,y); glyph is 3 wide × 5 tall */
static void draw_glyph(int16_t x, int16_t y, uint8_t idx) {
    if (idx >= 14) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = s_font[idx][row];
        for (int col = 0; col < 3; col++) {
            lcd_graphics_pixel(x + col, y + row, (bits >> (2 - col)) & 1);
        }
    }
}

/* Draw 2-digit decimal number (0–99) at pixel (x,y) using 4px glyph advance */
static void draw_num2(int16_t x, int16_t y, uint16_t n) {
    if (n > 99) n = 99;
    draw_glyph(x,     y, (uint8_t)(n / 10));
    draw_glyph(x + 4, y, (uint8_t)(n % 10));
}

/* Draw score line in top 7 px: "S:xx  H:xx" */
static void draw_score(uint16_t score, uint16_t hi) {
    /* Clear top area */
    for (int16_t y = 0; y < SNAKE_GAME_Y0 - 1; y++) {
        for (int16_t x = 0; x < SCREEN_W; x++) {
            lcd_graphics_pixel(x, y, false);
        }
    }
    int16_t x = 1;
    draw_glyph(x,      1, 10);  /* S */
    draw_glyph(x + 4,  1, 12);  /* : */
    draw_num2 (x + 8,  1, score);
    draw_glyph(x + 18, 1, 11);  /* H */
    draw_glyph(x + 22, 1, 12);  /* : */
    draw_num2 (x + 26, 1, hi);
    /* Separator line */
    for (int16_t px = 0; px < SCREEN_W; px++) {
        lcd_graphics_pixel(px, SNAKE_GAME_Y0 - 1, true);
    }
}

/*===========================================================================*/
/* Direction                                                                  */
/*===========================================================================*/

typedef enum { DIR_UP = 0, DIR_RIGHT, DIR_DOWN, DIR_LEFT } dir_t;

/* Relative turn: CW = right, CCW = left */
static dir_t turn_right(dir_t d) {
    return (dir_t)((d + 1) & 3);   /* UP→RIGHT→DOWN→LEFT→UP */
}
static dir_t turn_left(dir_t d) {
    return (dir_t)((d + 3) & 3);
}

/* Delta col/row for each direction */
static const int8_t s_dcol[4] = { 0,  1,  0, -1 };
static const int8_t s_drow[4] = {-1,  0,  1,  0 };

/*===========================================================================*/
/* Snake ring buffer                                                          */
/*===========================================================================*/

typedef struct { int8_t col, row; } cell_t;

static cell_t   s_body[SNAKE_MAX_LEN];
static uint16_t s_head;    /* index of head in ring buffer */
static uint16_t s_len;

/* Get cell i from head (0=head, len-1=tail) */
static cell_t snake_at(uint16_t i) {
    return s_body[(s_head + i) % SNAKE_MAX_LEN];
}

/* Push new head */
static void snake_push(cell_t c) {
    s_head = (s_head == 0) ? (SNAKE_MAX_LEN - 1) : (s_head - 1);
    s_body[s_head] = c;
    s_len++;
}

/* Pop tail */
static void snake_pop(void) {
    if (s_len > 0) s_len--;
}

/* True if cell (col, row) is occupied by snake */
static bool snake_hits(int8_t col, int8_t row) {
    for (uint16_t i = 0; i < s_len; i++) {
        cell_t c = snake_at(i);
        if (c.col == col && c.row == row) return true;
    }
    return false;
}

/*===========================================================================*/
/* Food placement — simple LCG                                               */
/*===========================================================================*/

static uint32_t s_rng;

static cell_t place_food(void) {
    cell_t f;
    do {
        s_rng = s_rng * 1664525UL + 1013904223UL;
        f.col = (int8_t)((s_rng >> 16) % (SNAKE_COLS - 2) + 1);
        f.row = (int8_t)((s_rng >> 22) % (SNAKE_ROWS - 2) + 1);
    } while (snake_hits(f.col, f.row));
    return f;
}

/*===========================================================================*/
/* Rendering                                                                  */
/*===========================================================================*/

/* Draw filled 3×3 square within a 4×4 cell (1px implicit gap) */
static void draw_cell_filled(int8_t col, int8_t row) {
    int16_t x = CELL_PX(col);
    int16_t y = CELL_PY(row);
    lcd_graphics_fill_rect(x, y, C - 1, C - 1, true);
}

/* Draw outlined 3×3 hollow square (food) */
static void draw_cell_outline(int8_t col, int8_t row) {
    int16_t x = CELL_PX(col);
    int16_t y = CELL_PY(row);
    lcd_graphics_rect(x, y, C - 2, C - 2, true);
}

static void render(uint16_t score, uint16_t hi, const cell_t *food) {
    lcd_graphics_clear();
    draw_score(score, hi);

    /* Border rect around game area */
    lcd_graphics_rect(GA_X0, GA_Y0, GA_X1 - GA_X0, GA_Y1 - GA_Y0, true);

    /* Food */
    draw_cell_outline(food->col, food->row);

    /* Snake body */
    for (uint16_t i = 0; i < s_len; i++) {
        cell_t c = snake_at(i);
        draw_cell_filled(c.col, c.row);
    }

    lcd_graphics_update();
}

/*===========================================================================*/
/* Char-mode game-over screen                                                 */
/*===========================================================================*/

static void show_game_over(uint16_t score, uint16_t hi) {
    lcd_graphics_mode(false);
    lcd_cmd(0x38); lcd_cmd(0x0C); lcd_cmd(0x06);
    lcd_clear();

    /* Build simple score message in char mode */
    char line[17];
    /* "Score: XX  Hi:XX" — manual formatting, no printf */
    const char *prefix = "Score:";
    int i = 0;
    while (prefix[i]) { line[i] = prefix[i]; i++; }
    line[i++] = (char)('0' + (score / 10) % 10);
    line[i++] = (char)('0' + score % 10);
    line[i++] = ' '; line[i++] = ' ';
    line[i++] = 'H'; line[i++] = 'i'; line[i++] = ':';
    line[i++] = (char)('0' + (hi / 10) % 10);
    line[i++] = (char)('0' + hi % 10);
    while (i < 16) { line[i++] = ' '; }
    line[16] = '\0';

    lcd_print_at(0, 0, "   GAME  OVER   ");
    lcd_print_at(1, 0, line);
    lcd_print_at(2, 0, "F1=RESTART      ");
    lcd_print_at(3, 0, "F2=MENU         ");
}

/*===========================================================================*/
/* snake_run                                                                  */
/*===========================================================================*/

void snake_run(void) {
    uint16_t hi_score = 0;

restart:
    s_rng = millis() ^ 12345;
    s_len  = 0;
    s_head = 0;
    uint8_t start_col = SNAKE_COLS / 2;
    uint8_t start_row = SNAKE_ROWS / 2;
    for (int8_t k = 2; k >= 0; k--) {
        cell_t c = { (int8_t)(start_col - k), (int8_t)start_row };
        snake_push(c);
    }

    dir_t    cur_dir   = DIR_RIGHT;
    cell_t   food      = place_food();
    uint16_t score     = 0;
    uint32_t tick_ms   = SNAKE_TICK_START;
    uint32_t last_tick = millis();
    bool     grow_next = false;   /* flag: don't pop tail next move */
    int8_t   input_delta = 0;     /* turn direction for this tick */

    lcd_graphics_mode(true);
    render(score, hi_score, &food);

    for (;;) {

        /* --- F2 exit --- */
        if (btn_f2_pressed()) {
            while (btn_f2_pressed()) { (void)millis(); delay_ms(10); }
            break;
        }

        /* --- F3/F4 speed control --- */
        if (btn_f3_pressed()) {
            while (btn_f3_pressed()) { (void)millis(); delay_ms(10); }
            if (tick_ms < 500) { tick_ms += 20; buzz(600, 20); }
        }
        if (btn_f4_pressed()) {
            while (btn_f4_pressed()) { (void)millis(); delay_ms(10); }
            if (tick_ms > SNAKE_TICK_MIN) { tick_ms -= 20; buzz(1000, 20); }
        }

        /* --- Accumulate encoder input --- */
        {
            int8_t enc = encoder_read_delta();
            if (enc > 0) input_delta =  1;
            else if (enc < 0) input_delta = -1;
        }

        /* --- Wait for tick --- */
        if ((millis() - last_tick) < tick_ms) { delay_ms(1); continue; }
        last_tick = millis();

        /* --- Apply input (one turn per tick, last direction wins) --- */
        if (input_delta > 0) {
            cur_dir = turn_right(cur_dir);
        } else if (input_delta < 0) {
            cur_dir = turn_left(cur_dir);
        }
        input_delta = 0;

        /* --- Compute new head --- */
        cell_t head   = snake_at(0);
        cell_t new_hd = {
            (int8_t)(head.col + s_dcol[cur_dir]),
            (int8_t)(head.row + s_drow[cur_dir])
        };

        /* --- Collision: wall (border cells are wall) --- */
        if (new_hd.col <= 0 || new_hd.col >= SNAKE_COLS - 1 ||
            new_hd.row <= 0 || new_hd.row >= SNAKE_ROWS - 1)
        {
            goto game_over;
        }

        /* --- Collision: self ---
         * When not growing the tail will be removed this tick, so the head
         * is allowed to move into the tail's position.  Only check body[0..len-2].
         * When growing (grow_next) the full body stays, check all of it. */
        {
            uint16_t check = grow_next ? s_len : (s_len > 0 ? s_len - 1u : 0u);
            for (uint16_t i = 0; i < check; i++) {
                cell_t c = snake_at(i);
                if (c.col == new_hd.col && c.row == new_hd.row) goto game_over;
            }
        }

        /* --- Move: pop tail (unless growing) --- */
        if (grow_next) {
            grow_next = false;
        } else {
            snake_pop();
        }

        /* --- Push new head --- */
        snake_push(new_hd);

        /* --- Food eaten? --- */
        if (new_hd.col == food.col && new_hd.row == food.row) {
            score++;
            if (score > hi_score) hi_score = score;
            if (tick_ms > SNAKE_TICK_MIN + SNAKE_TICK_STEP)
                tick_ms -= SNAKE_TICK_STEP;
            else
                tick_ms = SNAKE_TICK_MIN;
            grow_next = true;
            buzz(1000, 30);
            food = place_food();
        }

        render(score, hi_score, &food);
        continue;

game_over:
        buzz(500, 80);
        delay_ms(40);
        buzz(350, 80);
        delay_ms(40);
        buzz(200, 200);
        show_game_over(score, hi_score);

        /* Wait: F1=restart, F2=exit */
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

done:
    lcd_graphics_mode(false);
}

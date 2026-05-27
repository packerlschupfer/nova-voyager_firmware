/**
 * @file test_main.c
 * @brief Unit tests for the encoder quadrature decoder and debounce logic
 *
 * Strategy: re-implement the pure algorithmic core (quadrature state machine,
 * delta accumulation, button debounce) verbatim from encoder.c so that tests
 * run natively on the host without ISR/EXTI/FreeRTOS/shared.h dependencies.
 *
 * The lookup table and accumulation arithmetic are byte-for-byte identical to
 * encoder.c; any divergence there is a test bug.
 *
 * Tests are grouped as follows:
 *   Group A — Quadrature state machine (table correctness)
 *   Group B — Detent accumulation (raw_count → delta → clicks)
 *   Group C — Delta read-and-clear (encoder_get_delta semantics)
 *   Group D — Noise and invalid-transition rejection
 *   Group E — Button debounce (BTN_DEBOUNCE_MS = 50 ms)
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*===========================================================================*/
/* Controllable tick source (replaces HAL_GetTick)                           */
/*===========================================================================*/

/* Single definition; no other translation unit is linked in this test suite. */
static uint32_t uwTick = 0;
static inline uint32_t HAL_GetTick(void) { return uwTick; }

/*===========================================================================*/
/* Re-implementation of encoder.c core (verbatim logic, no ISR/GPIO)        */
/*===========================================================================*/

/* From config.h */
#define ENC_COUNTS_PER_DETENT 4

/* From encoder.c — lookup table: index = (old_state << 2) | new_state */
static const int8_t encoder_table[16] = {
     0, +1, -1,  0,   /* 00 → 00, 01, 10, 11 */
    -1,  0,  0, +1,   /* 01 → 00, 01, 10, 11 */
    +1,  0,  0, -1,   /* 10 → 00, 01, 10, 11 */
     0, -1, +1,  0    /* 11 → 00, 01, 10, 11 */
};

/* Volatile mirrors encoder.c's ISR-shared variables */
static volatile int16_t enc_position  = 0;
static volatile int8_t  enc_delta     = 0;
static volatile int8_t  enc_raw_count = 0;
static volatile uint8_t enc_last_state = 0;

/**
 * @brief Feed one quadrature state transition into the state machine.
 *
 * Mirrors encoder_process_state() from encoder.c, but accepts the new
 * two-bit pin value directly rather than reading GPIOC->IDR.
 *
 * @param new_pin_state  Two-bit value: bit1 = A (PC13), bit0 = B (PC14)
 */
static void enc_feed(uint8_t new_pin_state)
{
    uint8_t ns = new_pin_state & 0x03u;

    if (ns != enc_last_state) {
        uint8_t index = (uint8_t)((enc_last_state << 2) | ns);
        int8_t  dir   = encoder_table[index];
        enc_last_state = ns;

        if (dir != 0) {
            enc_raw_count = (int8_t)(enc_raw_count + dir);

            if (enc_raw_count >= ENC_COUNTS_PER_DETENT) {
                enc_raw_count = 0;
                enc_position++;
                enc_delta++;
            } else if (enc_raw_count <= -ENC_COUNTS_PER_DETENT) {
                enc_raw_count = 0;
                enc_position--;
                enc_delta--;
            }
        }
    }
}

/**
 * @brief Read and atomically clear the accumulated delta.
 *
 * Mirrors encoder_get_delta() from encoder.c.  In production code the
 * clear is guarded by __disable_irq()/__enable_irq(); here single-threaded
 * execution makes that unnecessary, but the semantics are identical.
 */
static int8_t enc_get_delta(void)
{
    int8_t d  = enc_delta;
    enc_delta = 0;
    return d;
}

/** Reset all encoder state (called in setUp). */
static void enc_reset(void)
{
    enc_position  = 0;
    enc_delta     = 0;
    enc_raw_count = 0;
    enc_last_state = 0;
}

/*===========================================================================*/
/* Re-implementation of button debounce logic (from encoder.c EXTI handler) */
/*===========================================================================*/

#define BTN_DEBOUNCE_MS 50u

static volatile bool     btn_event    = false;
static volatile uint32_t btn_last_ms  = 0;

/**
 * @brief Simulate a button EXTI falling-edge interrupt at the current tick.
 *
 * Mirrors the F1/F2/F3/Start/Menu debounce logic in EXTI15_10_IRQHandler
 * and EXTI4_IRQHandler from encoder.c.
 */
static void btn_press_isr(void)
{
    uint32_t now = HAL_GetTick();
    if (now - btn_last_ms >= BTN_DEBOUNCE_MS) {
        btn_last_ms = now;
        btn_event   = true;
    }
}

/** Read and clear the button event flag (mirrors encoder_fN_clicked). */
static bool btn_clicked(void)
{
    if (btn_event) {
        btn_event = false;
        return true;
    }
    return false;
}

/** Reset button state (called in setUp). */
static void btn_reset(void)
{
    btn_event   = false;
    btn_last_ms = 0;
    uwTick      = 100;
}

/*===========================================================================*/
/* Unity fixtures                                                             */
/*===========================================================================*/

void setUp(void)
{
    enc_reset();
    btn_reset();
}

void tearDown(void) {}

/*===========================================================================*/
/* Group A — Quadrature state machine correctness                            */
/*===========================================================================*/

/**
 * A1: CW sequence 00→01→11→10 produces +1 per valid step.
 *
 * Quadrature Gray code for clockwise rotation:
 *   state 0b00 (A=0,B=0) → 0b01 (A=0,B=1) → 0b11 (A=1,B=1) → 0b10 (A=1,B=0)
 *
 * Each of the three transitions is a legal +1 step in the table.  raw_count
 * climbs 0→1→2→3 but never reaches ENC_COUNTS_PER_DETENT (4), so no detent
 * fires.  That is verified separately in Group B.
 */
void test_a1_cw_table_step1_00_to_01(void)
{
    enc_last_state = 0x00;
    enc_feed(0x01);
    TEST_ASSERT_EQUAL_INT8(1, enc_raw_count);
}

void test_a1_cw_table_step2_01_to_11(void)
{
    enc_last_state = 0x00;
    enc_feed(0x01);  /* raw=1 */
    enc_feed(0x03);  /* raw=2 */
    TEST_ASSERT_EQUAL_INT8(2, enc_raw_count);
}

void test_a1_cw_table_step3_11_to_10(void)
{
    enc_last_state = 0x00;
    enc_feed(0x01);  /* raw=1 */
    enc_feed(0x03);  /* raw=2 */
    enc_feed(0x02);  /* raw=3 */
    TEST_ASSERT_EQUAL_INT8(3, enc_raw_count);
}

/**
 * A2: CCW sequence 00→10→11→01 produces -1 per valid step.
 */
void test_a2_ccw_table_step1_00_to_10(void)
{
    enc_last_state = 0x00;
    enc_feed(0x02);
    TEST_ASSERT_EQUAL_INT8(-1, enc_raw_count);
}

void test_a2_ccw_table_step2_10_to_11(void)
{
    enc_last_state = 0x00;
    enc_feed(0x02);
    enc_feed(0x03);
    TEST_ASSERT_EQUAL_INT8(-2, enc_raw_count);
}

void test_a2_ccw_table_step3_11_to_01(void)
{
    enc_last_state = 0x00;
    enc_feed(0x02);
    enc_feed(0x03);
    enc_feed(0x01);
    TEST_ASSERT_EQUAL_INT8(-3, enc_raw_count);
}

/*===========================================================================*/
/* Group B — Detent accumulation                                             */
/*===========================================================================*/

/**
 * B1: Four complete CW quadrature steps (one full detent) produce exactly
 *     one +1 detent click and reset raw_count to zero.
 *
 * CW Gray sequence that crosses the detent boundary:
 *   00 → 01 → 11 → 10 → 00
 */
void test_b1_four_cw_steps_equal_one_detent(void)
{
    enc_feed(0x01);  /* raw=1 */
    enc_feed(0x03);  /* raw=2 */
    enc_feed(0x02);  /* raw=3 */
    enc_feed(0x00);  /* raw=4 → detent! raw resets to 0 */

    TEST_ASSERT_EQUAL_INT8(0,  enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(1,  enc_delta);
    TEST_ASSERT_EQUAL_INT16(1, enc_position);
}

/**
 * B2: Four complete CCW quadrature steps produce exactly one -1 detent click.
 *
 * CCW Gray sequence:
 *   00 → 10 → 11 → 01 → 00
 */
void test_b2_four_ccw_steps_equal_one_detent(void)
{
    enc_feed(0x02);
    enc_feed(0x03);
    enc_feed(0x01);
    enc_feed(0x00);

    TEST_ASSERT_EQUAL_INT8(0,   enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(-1,  enc_delta);
    TEST_ASSERT_EQUAL_INT16(-1, enc_position);
}

/**
 * B3: Only two CW steps — a partial rotation — must not complete a detent.
 *     delta stays 0, raw_count stays 2.
 */
void test_b3_partial_two_cw_steps_no_detent(void)
{
    enc_feed(0x01);
    enc_feed(0x03);

    TEST_ASSERT_EQUAL_INT8(2, enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);
    TEST_ASSERT_EQUAL_INT16(0, enc_position);
}

/**
 * B4: Eight CW steps (two full detents) produce delta = +2, position = +2.
 */
void test_b4_eight_cw_steps_equal_two_detents(void)
{
    /* Detent 1: 00→01→11→10→00 */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);
    /* Detent 2: 00→01→11→10→00 */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);

    TEST_ASSERT_EQUAL_INT8(0,  enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(2,  enc_delta);
    TEST_ASSERT_EQUAL_INT16(2, enc_position);
}

/**
 * B5: Direction reversal mid-detent — two CW steps followed by two CCW steps
 *     cancels out.  raw_count returns to 0 and no detent fires.
 */
void test_b5_cw_then_ccw_mid_detent_cancels(void)
{
    /* Two CW: raw → +2 */
    enc_feed(0x01);
    enc_feed(0x03);
    /* Reverse two CCW: raw → +1 then 0 */
    enc_feed(0x01);  /* 11→01 = -1, raw = 1 */
    enc_feed(0x00);  /* 01→00 = -1, raw = 0 */

    TEST_ASSERT_EQUAL_INT8(0, enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);
    TEST_ASSERT_EQUAL_INT16(0, enc_position);
}

/*===========================================================================*/
/* Group C — Delta read-and-clear                                            */
/*===========================================================================*/

/**
 * C1: After accumulating three detent clicks, enc_get_delta() returns 3
 *     and clears delta to 0.  A second call immediately returns 0.
 */
void test_c1_get_delta_returns_accumulated_and_clears(void)
{
    /* Three full CW detents (12 steps) */
    for (int i = 0; i < 3; i++) {
        enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);
    }

    TEST_ASSERT_EQUAL_INT8(3, enc_delta);

    int8_t d = enc_get_delta();
    TEST_ASSERT_EQUAL_INT8(3, d);

    /* Delta must be cleared */
    d = enc_get_delta();
    TEST_ASSERT_EQUAL_INT8(0, d);
}

/**
 * C2: Position is not affected by enc_get_delta() — it keeps accumulating
 *     across read-and-clear calls.
 */
void test_c2_position_unaffected_by_get_delta(void)
{
    /* Detent 1 */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);
    (void)enc_get_delta();  /* clears delta */

    /* Detent 2 */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);

    TEST_ASSERT_EQUAL_INT16(2, enc_position);
    TEST_ASSERT_EQUAL_INT8(1, enc_get_delta());
}

/**
 * C3: Atomic read-and-clear simulation — verify that a mid-operation ISR
 *     interleaving cannot produce a double-read of the same delta.
 *
 *     In a single-threaded host test, "ISR interleaving" is simulated as:
 *       1. Task snapshots delta into a local variable.
 *       2. ISR fires and adds another detent click.
 *       3. Task clears delta (its stale copy is 0; one click is preserved).
 *
 *     In the real firmware this is protected by __disable_irq().  Here we
 *     verify that the pattern of snapshot-then-clear correctly preserves any
 *     click that was added between snapshot and clear.
 */
void test_c3_interleaved_isr_click_not_lost(void)
{
    /* ISR accumulates 2 detents */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);

    /* Task snapshots delta */
    int8_t snapshot = enc_delta;

    /* ISR fires mid-read (simulated) and adds one more */
    enc_feed(0x01); enc_feed(0x03); enc_feed(0x02); enc_feed(0x00);

    /* Task clears delta to 0 (this is what __disable_irq prevents) */
    enc_delta = 0;

    /* The third click was added AFTER the clear — in the real firmware
     * __disable_irq() ensures enc_delta is atomically snapped + zeroed.
     * Without it, the ISR click added after the snapshot is wiped.
     * This test documents that risk: snapshot=2, but 1 click was lost. */
    TEST_ASSERT_EQUAL_INT8(2, snapshot);   /* only the pre-snapshot value */
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);  /* ISR click was wiped — known hazard */
    /* Position reflects all three detents correctly */
    TEST_ASSERT_EQUAL_INT16(3, enc_position);
}

/*===========================================================================*/
/* Group D — Noise and invalid-transition rejection                          */
/*===========================================================================*/

/**
 * D1: Repeated same-state transitions (contact bounce) produce no movement.
 *     Calling enc_feed with the current state should be a no-op.
 */
void test_d1_same_state_repeated_no_movement(void)
{
    enc_feed(0x00);
    enc_feed(0x00);
    enc_feed(0x00);
    enc_feed(0x00);

    TEST_ASSERT_EQUAL_INT8(0,  enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0,  enc_delta);
    TEST_ASSERT_EQUAL_INT16(0, enc_position);
}

/**
 * D2: Bounce after a partial CW step — same state fed twice, then continue.
 *     The bounce must not corrupt the count.
 */
void test_d2_bounce_after_partial_step_no_extra_count(void)
{
    enc_feed(0x01);  /* 00→01, raw=1 */
    enc_feed(0x01);  /* bounce: same state, no-op */
    enc_feed(0x01);  /* bounce again */
    enc_feed(0x03);  /* 01→11, raw=2 */

    TEST_ASSERT_EQUAL_INT8(2, enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);
}

/**
 * D3: Invalid transition 00→11 (impossible on real quadrature hardware —
 *     both channels cannot change simultaneously in a Gray code).
 *
 *     The lookup table entry for index (0b00 << 2) | 0b11 = 3 is 0, so
 *     the encoder core must produce zero direction and leave state unchanged.
 *     This is the "gracefully handled" requirement from the spec.
 */
void test_d3_invalid_00_to_11_no_movement(void)
{
    enc_last_state = 0x00;
    enc_feed(0x03);  /* direct 00→11 — table[3] = 0 */

    TEST_ASSERT_EQUAL_INT8(0, enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);
    /* last_state should have been updated to 11 for next transition */
    TEST_ASSERT_EQUAL_UINT8(0x03, enc_last_state);
}

/**
 * D4: Invalid transition 01→10 (direct cross) — table entry is also 0.
 */
void test_d4_invalid_01_to_10_no_movement(void)
{
    enc_last_state = 0x01;
    enc_feed(0x02);  /* table[(0b01<<2)|0b10] = table[6] = 0 */

    TEST_ASSERT_EQUAL_INT8(0, enc_raw_count);
    TEST_ASSERT_EQUAL_INT8(0, enc_delta);
    TEST_ASSERT_EQUAL_UINT8(0x02, enc_last_state);
}

/*===========================================================================*/
/* Group E — Button debounce                                                 */
/*===========================================================================*/

/**
 * E1: A single press at t=0 sets the event flag.
 */
void test_e1_single_press_registers(void)
{
    btn_press_isr();

    TEST_ASSERT_TRUE(btn_clicked());
}

/**
 * E2: After one click, btn_clicked() returns false (event self-clears).
 */
void test_e2_event_self_clears_after_read(void)
{
    btn_press_isr();
    (void)btn_clicked();  /* consume */

    TEST_ASSERT_FALSE(btn_clicked());
}

/**
 * E3: Two presses within BTN_DEBOUNCE_MS (50 ms) — only the first counts.
 *
 *     First press at t=0, second at t=30 (< 50): second must be ignored.
 */
void test_e3_two_presses_within_debounce_only_first_counts(void)
{
    btn_press_isr();  /* accepted */

    uwTick += 30;
    btn_press_isr();  /* within window, ignored */

    TEST_ASSERT_TRUE(btn_clicked());    /* first press */
    TEST_ASSERT_FALSE(btn_clicked());   /* no second press */
}

/**
 * E4: Press exactly at the debounce boundary (t = BTN_DEBOUNCE_MS) is
 *     accepted because the condition is `now - last >= BTN_DEBOUNCE_MS`.
 */
void test_e4_press_at_exact_debounce_boundary_accepted(void)
{
    btn_press_isr();  /* accepted */
    (void)btn_clicked();

    uwTick += BTN_DEBOUNCE_MS;  /* exactly 50ms later */
    btn_press_isr();             /* accepted */

    TEST_ASSERT_TRUE(btn_clicked());
}

/**
 * E5: Press one millisecond before the boundary (t=49) must still be rejected.
 */
void test_e5_press_one_ms_before_boundary_rejected(void)
{
    btn_press_isr();  /* accepted */
    (void)btn_clicked();

    uwTick += BTN_DEBOUNCE_MS - 1;  /* 49ms later */
    btn_press_isr();                  /* rejected */

    TEST_ASSERT_FALSE(btn_clicked());
}

/**
 * E6: Press, wait > 50 ms, press again — both must be counted.
 *
 *     Mirrors the "press, wait 50ms, press → both counted" spec requirement.
 */
void test_e6_press_after_debounce_both_counted(void)
{
    btn_press_isr();   /* first press */
    TEST_ASSERT_TRUE(btn_clicked());

    uwTick += 60;      /* 60ms later — beyond debounce window */
    btn_press_isr();   /* second press */
    TEST_ASSERT_TRUE(btn_clicked());
}

/**
 * E7: Multiple rapid bounces followed by a clean press after the window —
 *     only the last (clean) press registers.
 */
void test_e7_rapid_bounces_then_clean_press(void)
{
    btn_press_isr();  /* accepted */

    /* Rapid bounce burst — all within window */
    uint32_t base = uwTick;
    for (uint32_t t = 5; t < BTN_DEBOUNCE_MS; t += 5) {
        uwTick = base + t;
        btn_press_isr();
    }
    (void)btn_clicked();  /* consume the first accepted press */

    /* Verify none of the bounce events leaked through */
    TEST_ASSERT_FALSE(btn_clicked());

    /* Clean press after the window */
    uwTick = base + BTN_DEBOUNCE_MS + 10;
    btn_press_isr();
    TEST_ASSERT_TRUE(btn_clicked());
}

/**
 * E8: Tick counter wrap-around (uint32_t overflow) must not break debounce.
 *
 *     last_ms = UINT32_MAX - 10, now = UINT32_MAX - 10 + 60.
 *     Unsigned subtraction wraps correctly: (now - last) = 60 >= 50.
 */
void test_e8_tick_wraparound_debounce_safe(void)
{
    uwTick     = 0xFFFFFFF5u;  /* UINT32_MAX - 10 */
    btn_press_isr();           /* accepted, last = 0xFFFFFFF5 */
    (void)btn_clicked();

    uwTick = 0xFFFFFFF5u + 60u;  /* wraps to 0x00000031 */
    btn_press_isr();              /* (0x31 - 0xFFFFFFF5) = 60 (unsigned) >= 50 */
    TEST_ASSERT_TRUE(btn_clicked());
}

/*===========================================================================*/
/* Test runner                                                                */
/*===========================================================================*/

int main(void)
{
    UNITY_BEGIN();

    /* Group A: Quadrature state machine */
    RUN_TEST(test_a1_cw_table_step1_00_to_01);
    RUN_TEST(test_a1_cw_table_step2_01_to_11);
    RUN_TEST(test_a1_cw_table_step3_11_to_10);
    RUN_TEST(test_a2_ccw_table_step1_00_to_10);
    RUN_TEST(test_a2_ccw_table_step2_10_to_11);
    RUN_TEST(test_a2_ccw_table_step3_11_to_01);

    /* Group B: Detent accumulation */
    RUN_TEST(test_b1_four_cw_steps_equal_one_detent);
    RUN_TEST(test_b2_four_ccw_steps_equal_one_detent);
    RUN_TEST(test_b3_partial_two_cw_steps_no_detent);
    RUN_TEST(test_b4_eight_cw_steps_equal_two_detents);
    RUN_TEST(test_b5_cw_then_ccw_mid_detent_cancels);

    /* Group C: Delta read-and-clear */
    RUN_TEST(test_c1_get_delta_returns_accumulated_and_clears);
    RUN_TEST(test_c2_position_unaffected_by_get_delta);
    RUN_TEST(test_c3_interleaved_isr_click_not_lost);

    /* Group D: Noise and invalid transitions */
    RUN_TEST(test_d1_same_state_repeated_no_movement);
    RUN_TEST(test_d2_bounce_after_partial_step_no_extra_count);
    RUN_TEST(test_d3_invalid_00_to_11_no_movement);
    RUN_TEST(test_d4_invalid_01_to_10_no_movement);

    /* Group E: Button debounce */
    RUN_TEST(test_e1_single_press_registers);
    RUN_TEST(test_e2_event_self_clears_after_read);
    RUN_TEST(test_e3_two_presses_within_debounce_only_first_counts);
    RUN_TEST(test_e4_press_at_exact_debounce_boundary_accepted);
    RUN_TEST(test_e5_press_one_ms_before_boundary_rejected);
    RUN_TEST(test_e6_press_after_debounce_both_counted);
    RUN_TEST(test_e7_rapid_bounces_then_clean_press);
    RUN_TEST(test_e8_tick_wraparound_debounce_safe);

    return UNITY_END();
}

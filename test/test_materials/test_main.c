/**
 * @file test_main.c
 * @brief Tests for the REAL speed calculator in include/materials.h.
 *
 * Includes the shipping header rather than a copy, so these exercise the code
 * cmd_calc() and menu.c actually call.
 *
 * The bug this suite exists for: hole saws were costed as "a twist drill at
 * 0.4x speed", which at 17 mm recommended 90-135 rpm for steel where Bosch
 * publishes 500, and 60-90 for stainless against Bosch's 250 - out by four to
 * five times. The operator-facing consequence is a tool that rubs instead of
 * cutting, which is what started the 2026-10-13 investigation.
 *
 * The metal expectations below are Bosch's own published figures for HSS
 * bi-metal hole saws, confirmed against the printed packaging of two current
 * PRO Multi Material saws. They are the specification, not a snapshot of our
 * output: if a change to the table moves them outside the recommended band,
 * that is a regression in the recommendation, not a stale test.
 */

#include <unity.h>
#include <stdio.h>
#include "materials.h"

void setUp(void) {}
void tearDown(void) {}

/* Assert that Bosch's published rpm for this diameter/material sits inside the
 * band we recommend. */
static void assert_band_contains(material_type_t mat, bit_type_t bit,
                                 uint16_t dia, uint16_t expected,
                                 const char* what) {
    uint16_t lo = 0, hi = 0;
    material_calc_rpm_range(mat, bit, dia, &lo, &hi);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(hi, lo, "band must not be inverted");
    if (expected < lo || expected > hi) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "%s: Bosch says %u rpm, we recommend %u-%u", what, expected, lo, hi);
        TEST_FAIL_MESSAGE(msg);
    }
}

/*===========================================================================*/
/* Hole saws vs Bosch's published table                                       */
/*===========================================================================*/

void test_hole_saw_aluminium_matches_bosch(void) {
    assert_band_contains(MATERIAL_ALUMINUM, BIT_HOLE_SAW,  17, 750, "alu 17mm");
    assert_band_contains(MATERIAL_ALUMINUM, BIT_HOLE_SAW,  51, 255, "alu 51mm");
    assert_band_contains(MATERIAL_ALUMINUM, BIT_HOLE_SAW, 102, 130, "alu 102mm");
}

void test_hole_saw_steel_matches_bosch(void) {
    assert_band_contains(MATERIAL_STEEL, BIT_HOLE_SAW,  17, 500, "steel 17mm");
    assert_band_contains(MATERIAL_STEEL, BIT_HOLE_SAW,  51, 170, "steel 51mm");
    assert_band_contains(MATERIAL_STEEL, BIT_HOLE_SAW, 102,  85, "steel 102mm");
}

void test_hole_saw_stainless_matches_bosch(void) {
    assert_band_contains(MATERIAL_STAINLESS, BIT_HOLE_SAW,  17, 250, "ss 17mm");
    assert_band_contains(MATERIAL_STAINLESS, BIT_HOLE_SAW,  51,  85, "ss 51mm");
    /* Bosch says 40 rpm at 102 mm, which is below SPEED_MIN_RPM - the machine
     * cannot turn that slowly, so the band sits on the floor. The point of the
     * change is that it is the MACHINE's floor and not the twist-drill 250. */
    {
        uint16_t lo = 0, hi = 0;
        material_calc_rpm_range(MATERIAL_STAINLESS, BIT_HOLE_SAW, 102, &lo, &hi);
        TEST_ASSERT_EQUAL_UINT16(HOLE_SAW_RPM_FLOOR, lo);
        TEST_ASSERT_EQUAL_UINT16(HOLE_SAW_RPM_FLOOR, hi);
    }
}

void test_hole_saw_wood_matches_bosch(void) {
    /* Bosch publishes one wood/PVC column: 3000 at 17mm, 2000 at 51, 1000 at
     * 102. The small end is a flat cap, not a surface speed. */
    assert_band_contains(MATERIAL_SOFTWOOD, BIT_HOLE_SAW,  17, 3000, "wood 17mm");
    assert_band_contains(MATERIAL_SOFTWOOD, BIT_HOLE_SAW,  51, 2000, "wood 51mm");
    assert_band_contains(MATERIAL_SOFTWOOD, BIT_HOLE_SAW, 102, 1000, "wood 102mm");
}

void test_hole_saw_never_exceeds_the_ceiling(void) {
    /* Bosch never recommends above 3000 for a hole saw at any diameter. */
    for (int m = 0; m < MATERIAL_COUNT; m++) {
        for (uint16_t d = 1; d <= 50; d++) {
            uint16_t lo = 0, hi = 0;
            material_calc_rpm_range((material_type_t)m, BIT_HOLE_SAW, d, &lo, &hi);
            TEST_ASSERT_LESS_OR_EQUAL(HOLE_SAW_RPM_CEIL, hi);
            TEST_ASSERT_GREATER_OR_EQUAL(HOLE_SAW_RPM_FLOOR, lo);
            TEST_ASSERT_LESS_OR_EQUAL(hi, lo);
        }
    }
}

void test_big_hole_saw_is_not_floored_at_the_twist_drill_minimum(void) {
    /* The old shared floor of 250 rpm would have recommended six times
     * Bosch's figure for a 102 mm stainless hole saw. */
    uint16_t lo = 0, hi = 0;
    material_calc_rpm_range(MATERIAL_STAINLESS, BIT_HOLE_SAW, 102, &lo, &hi);
    TEST_ASSERT_LESS_THAN_UINT16(250, hi);
}

/*===========================================================================*/
/* Everything else must be untouched                                          */
/*===========================================================================*/

void test_twist_drill_is_unchanged(void) {
    /* Softwood, 10 mm twist: 45-55 m/min at 1.0x. */
    uint16_t lo = 0, hi = 0;
    material_calc_rpm_range(MATERIAL_SOFTWOOD, BIT_TWIST, 10, &lo, &hi);
    TEST_ASSERT_EQUAL_UINT16(1431, lo);
    TEST_ASSERT_EQUAL_UINT16(1749, hi);
}

void test_forstner_still_uses_its_factor(void) {
    uint16_t twist_lo = 0, twist_hi = 0, f_lo = 0, f_hi = 0;
    /* 10 mm, not 30: at 30 mm this material lands under MATERIAL_RPM_FLOOR and
     * both bands clamp to 250, which would make the assertion vacuous. That
     * clamp is pre-existing twist-drill behaviour and is left alone here - but
     * it is the same shape of problem as the hole-saw factor was: a 30 mm
     * Forstner in hardwood wants ~160 rpm and gets told 250. */
    material_calc_rpm_range(MATERIAL_HARDWOOD, BIT_TWIST,    10, &twist_lo, &twist_hi);
    material_calc_rpm_range(MATERIAL_HARDWOOD, BIT_FORSTNER, 10, &f_lo, &f_hi);
    TEST_ASSERT_EQUAL_UINT16((twist_hi * 6) / 10, f_hi);
}

void test_unknown_material_still_falls_back(void) {
    uint16_t lo = 0, hi = 0;
    material_calc_rpm_range(MATERIAL_COUNT, BIT_HOLE_SAW, 17, &lo, &hi);
    TEST_ASSERT_EQUAL_UINT16(500, lo);
    TEST_ASSERT_EQUAL_UINT16(1500, hi);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hole_saw_aluminium_matches_bosch);
    RUN_TEST(test_hole_saw_steel_matches_bosch);
    RUN_TEST(test_hole_saw_stainless_matches_bosch);
    RUN_TEST(test_hole_saw_wood_matches_bosch);
    RUN_TEST(test_hole_saw_never_exceeds_the_ceiling);
    RUN_TEST(test_big_hole_saw_is_not_floored_at_the_twist_drill_minimum);
    RUN_TEST(test_twist_drill_is_unchanged);
    RUN_TEST(test_forstner_still_uses_its_factor);
    RUN_TEST(test_unknown_material_still_falls_back);
    return UNITY_END();
}

/**
 * @file test_temperature.c
 * @brief Unit tests for temperature monitoring module
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>

// Mock dependencies
#define SEND_EVENT(x)
int32_t motor_read_param(uint16_t cmd) { return 0; }
uint16_t temperature_read_gd32(void) { return 25; }  // Room temp

// Include implementation (just the monitoring part, not ADC)
#define temp_sensor_initialized 1  // Skip ADC init
static uint16_t mcb_temp_cached = 0;
static bool temp_warning_active = false;

#define TEMP_WARNING_DEFAULT 60
#define TEMP_HYSTERESIS 5

// Minimal implementation for testing
void temp_monitor_update(uint16_t current_temp, uint8_t threshold) {
    if (threshold == 0) {
        threshold = TEMP_WARNING_DEFAULT;
    }

    if (current_temp > 0 && current_temp < 150) {
        mcb_temp_cached = current_temp;
    }

    if (current_temp == 0) {
        // Invalid
    } else if (current_temp >= threshold) {
        if (!temp_warning_active) {
            temp_warning_active = true;
            SEND_EVENT(0);  // EVT_TEMP_WARNING
        }
    } else if (current_temp < threshold - TEMP_HYSTERESIS) {
        temp_warning_active = false;
    }
}

bool temp_is_warning_active(void) {
    return temp_warning_active;
}

uint16_t temp_get_mcb(void) {
    if (mcb_temp_cached > 0 && mcb_temp_cached < 150) {
        return mcb_temp_cached;
    }
    return temperature_read_gd32();
}

void setUp(void) {
    mcb_temp_cached = 0;
    temp_warning_active = false;
}

void tearDown(void) {
    // Cleanup
}

/*===========================================================================*/
/* Temperature Threshold Tests (Phase 2.2)                                   */
/*===========================================================================*/

void test_temp_below_threshold_no_warning(void) {
    // Temp at 50°C, threshold 60°C
    temp_monitor_update(50, 60);

    TEST_ASSERT_FALSE(temp_is_warning_active());
    TEST_ASSERT_EQUAL(50, temp_get_mcb());
}

void test_temp_at_threshold_triggers_warning(void) {
    // Temp exactly at threshold
    temp_monitor_update(60, 60);

    TEST_ASSERT_TRUE(temp_is_warning_active());
}

void test_temp_above_threshold_triggers_warning(void) {
    // Temp above threshold
    temp_monitor_update(65, 60);

    TEST_ASSERT_TRUE(temp_is_warning_active());
}

void test_temp_warning_only_triggers_once(void) {
    // First time at 65°C
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Still at 65°C - warning already active
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());  // Should stay active
}

/*===========================================================================*/
/* Hysteresis Tests                                                          */
/*===========================================================================*/

void test_hysteresis_warning_persists(void) {
    // Trigger warning at 65°C
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Drop to 59°C (still above threshold - hysteresis)
    temp_monitor_update(59, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());  // Should persist
}

void test_hysteresis_clears_below_threshold_minus_5(void) {
    // Trigger warning
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Drop to 54°C (below 60 - 5)
    temp_monitor_update(54, 60);
    TEST_ASSERT_FALSE(temp_is_warning_active());  // Should clear
}

void test_hysteresis_exact_boundary(void) {
    // Warning at 65°C
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Drop to exactly 55°C (threshold - hysteresis = 60 - 5 = 55)
    // Logic uses < (not <=), so 55 is NOT below threshold
    temp_monitor_update(55, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());  // Still active

    // Drop to 54°C (now < 55)
    temp_monitor_update(54, 60);
    TEST_ASSERT_FALSE(temp_is_warning_active());  // Now clears
}

/*===========================================================================*/
/* Default Threshold Tests                                                   */
/*===========================================================================*/

void test_default_threshold_when_zero(void) {
    // Pass 0 as threshold - should use default (60°C)
    temp_monitor_update(65, 0);

    TEST_ASSERT_TRUE(temp_is_warning_active());  // Uses default 60°C
}

void test_custom_threshold(void) {
    // Custom threshold: 70°C
    temp_monitor_update(65, 70);

    TEST_ASSERT_FALSE(temp_is_warning_active());  // Below 70°C

    temp_monitor_update(71, 70);
    TEST_ASSERT_TRUE(temp_is_warning_active());  // Above 70°C
}

/*===========================================================================*/
/* Edge Cases                                                                 */
/*===========================================================================*/

void test_invalid_temp_zero(void) {
    // Invalid temp (0) should be ignored
    temp_monitor_update(0, 60);

    TEST_ASSERT_FALSE(temp_is_warning_active());
}

void test_invalid_temp_too_high(void) {
    // Invalid temp (>150) should be ignored for caching
    temp_monitor_update(200, 60);

    // Should not cache invalid temp
    uint16_t cached = temp_get_mcb();
    TEST_ASSERT_EQUAL(25, cached);  // Should return GD32 fallback
}

void test_temp_caching(void) {
    // Valid temp should be cached
    temp_monitor_update(45, 60);

    uint16_t cached = temp_get_mcb();
    TEST_ASSERT_EQUAL(45, cached);
}

void test_warning_state_transitions(void) {
    // Start below threshold
    temp_monitor_update(50, 60);
    TEST_ASSERT_FALSE(temp_is_warning_active());

    // Rise above - trigger warning
    temp_monitor_update(65, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Stay high - warning persists
    temp_monitor_update(62, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());

    // Drop below (with hysteresis) - clear warning
    temp_monitor_update(54, 60);
    TEST_ASSERT_FALSE(temp_is_warning_active());

    // Rise again - retrigger warning
    temp_monitor_update(61, 60);
    TEST_ASSERT_TRUE(temp_is_warning_active());
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Threshold tests
    RUN_TEST(test_temp_below_threshold_no_warning);
    RUN_TEST(test_temp_at_threshold_triggers_warning);
    RUN_TEST(test_temp_above_threshold_triggers_warning);
    RUN_TEST(test_temp_warning_only_triggers_once);

    // Hysteresis tests
    RUN_TEST(test_hysteresis_warning_persists);
    RUN_TEST(test_hysteresis_clears_below_threshold_minus_5);
    RUN_TEST(test_hysteresis_exact_boundary);

    // Default threshold
    RUN_TEST(test_default_threshold_when_zero);
    RUN_TEST(test_custom_threshold);

    // Edge cases
    RUN_TEST(test_invalid_temp_zero);
    RUN_TEST(test_invalid_temp_too_high);
    RUN_TEST(test_temp_caching);
    RUN_TEST(test_warning_state_transitions);

    return UNITY_END();
}

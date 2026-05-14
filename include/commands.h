/**
 * @file commands.h
 * @brief Console command handlers for Nova Voyager firmware
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Command Handler Type                                                      */
/*===========================================================================*/

typedef void (*cmd_handler_t)(void);

typedef struct {
    const char* name;
    cmd_handler_t handler;
    uint8_t flags;
} cmd_entry_t;

#define CMD_FLAG_DEBUG  0x01

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize command processing
 */
void commands_init(void);

/**
 * @brief Process a single character from serial input
 * @param c Character to process
 */
void commands_process_char(int c);

/**
 * @brief Check for and process any pending serial commands
 */
void commands_check_serial(void);

/**
 * @brief Get pointer to command table
 * @return Pointer to command table array
 */
const cmd_entry_t* commands_get_table(void);

/**
 * @brief Get size of command table
 * @return Number of commands in table
 */
uint32_t commands_get_table_size(void);

/*===========================================================================*/
/* Command Handlers (exported for direct invocation if needed)              */
/*===========================================================================*/

// System commands
/** @brief Enter DFU bootloader mode */
void cmd_dfu(void);
/** @brief Reset the system */
void cmd_reset(void);
/** @brief Save settings to EEPROM */
void cmd_save(void);
/** @brief Display crash dump information */
void cmd_crashshow(void);
/** @brief Clear crash dump data */
void cmd_crashclear(void);
/** @brief Show available commands */
void cmd_help(void);
/** @brief Show system status */
void cmd_status(void);

// Motor commands
/** @brief Query motor GF (get flags) */
void cmd_gf(void);
/** @brief Send motor RS (stop) command */
void cmd_rs(void);
/** @brief Query motor parameter */
void cmd_mq(void);
/** @brief Sync motor parameters */
void cmd_msync(void);
/** @brief Save motor parameters to MCB */
void cmd_msave(void);
/** @brief Read motor parameters from MCB */
void cmd_mread(void);

// Menu/UI commands
/** @brief Enter menu mode */
void cmd_menu(void);
/** @brief Navigate menu up */
void cmd_up(void);
/** @brief Navigate menu down */
void cmd_dn(void);
/** @brief Select menu item */
void cmd_ok(void);
/** @brief Trigger F1 button action */
void cmd_f1(void);
/** @brief Trigger F2 button action */
void cmd_f2(void);
/** @brief Trigger F3 button action */
void cmd_f3(void);
/** @brief Trigger F4 button action */
void cmd_f4(void);

// Hardware test commands
/** @brief Show depth sensor status */
void cmd_depth(void);
/** @brief Show guard sensor status */
void cmd_guard(void);
/** @brief Monitor ADC values */
void cmd_adcmon(void);
/** @brief Show task stack usage */
void cmd_stack(void);
/** @brief Show temperature readings */
void cmd_temp(void);
/** @brief Test calculation functions */
void cmd_calc(void);
/** @brief Run hardware self-test */
void cmd_selftest(void);
/** @brief Scan I2C bus */
void cmd_i2c(void);
/** @brief Read ADC channels */
void cmd_adc(void);
/** @brief Test buzzer */
void cmd_buzz(void);
/** @brief LCD hardware test */
void cmd_lcd(void);

// Tapping commands
void cmd_tap(void);
void cmd_tapload(void);
void cmd_taprev(void);
void cmd_tappeck(void);
void cmd_tapact(void);
void cmd_tapthr(void);
void cmd_tapbrk(void);

// Debug-only commands
#ifdef BUILD_DEBUG
void cmd_enc(void);
void cmd_setdbg(void);
void cmd_qq(void);
void cmd_scan(void);
void cmd_listen(void);
void cmd_gscan(void);
void cmd_taptest(void);
void cmd_tapstop(void);
void cmd_tapsim(void);
void cmd_test(void);
void cmd_testcl(void);
#endif

#endif // COMMANDS_H

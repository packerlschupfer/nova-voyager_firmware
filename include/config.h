/**
 * @file config.h
 * @brief Nova Voyager Hardware Configuration
 *
 * Pin mappings and hardware constants derived from reverse engineering
 * the original Teknatool firmware.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "stm32f1xx_hal.h"

/*===========================================================================*/
/* System Configuration                                                       */
/*===========================================================================*/

// Set to 1 for 120MHz (GD32F303 max), 0 for 72MHz (STM32 compatible)
// Can be overridden via platformio.ini build flags
#ifndef USE_120MHZ
#define USE_120MHZ          0  // Default: 72MHz (STM32 compatible)
#endif

#if USE_120MHZ
#define SYSCLK_FREQ         120000000   // 120MHz from HSE*15
#define APB1_FREQ           30000000    // 120MHz / 4
#define APB2_FREQ           120000000   // 120MHz / 1
#else
#define SYSCLK_FREQ         72000000    // 72MHz from HSE*9
#define APB1_FREQ           36000000    // 72MHz / 2
#define APB2_FREQ           72000000    // 72MHz / 1
#endif

#define HSE_VALUE           8000000     // 8MHz external crystal

/*===========================================================================*/
/* Debug Output Configuration                                                 */
/*===========================================================================*/

// Enable debug UART output (disabled in production builds)
// Can be overridden via platformio.ini build flags (-DENABLE_DEBUG_OUTPUT=1)
#ifndef ENABLE_DEBUG_OUTPUT
  #ifdef NDEBUG
    #define ENABLE_DEBUG_OUTPUT 0  // Production: No debug output
  #else
    #define ENABLE_DEBUG_OUTPUT 1  // Debug: Enable all output
  #endif
#endif

// Debug output macros - compile out completely when disabled
#if ENABLE_DEBUG_OUTPUT
  extern void uart_puts(const char* s);
  extern void uart_putc(char c);
  extern void print_num(int32_t n);

  #define DEBUG_PRINT(msg)        uart_puts(msg)
  #define DEBUG_PRINTC(ch)        uart_putc(ch)
  #define DEBUG_PRINTNUM(num)     print_num(num)
#else
  // No-ops that compile to nothing
  #define DEBUG_PRINT(msg)        ((void)0)
  #define DEBUG_PRINTC(ch)        ((void)0)
  #define DEBUG_PRINTNUM(num)     ((void)0)
#endif

/*===========================================================================*/
/* Motor Controller Communication                                             */
/*===========================================================================*/

// UART to motor controller (USART3 on PB10/PB11 - verified from hardware)
#define MOTOR_UART          USART3
#define MOTOR_UART_BAUD     9600        // Verified from original firmware
#define MOTOR_TX_PIN        GPIO_PIN_10
#define MOTOR_TX_PORT       GPIOB
#define MOTOR_RX_PIN        GPIO_PIN_11
#define MOTOR_RX_PORT       GPIOB

// Motor command retry configuration
#define MOTOR_RETRY_MAX             3       // Maximum retry attempts
#define MOTOR_RETRY_DELAY_MS        50      // Initial retry delay (exponential backoff)

// UART timeout configuration (Phase 1.1 safety improvement)
#define MOTOR_UART_TX_TIMEOUT_MS    100     // TX complete timeout (milliseconds)
#define MOTOR_UART_RX_TIMEOUT_MS    100     // RX byte timeout (milliseconds)
#define MOTOR_UART_BYTE_TIMEOUT_MS  10      // Individual byte transmission timeout

// Motor command codes (ASCII) - Verified from original Teknatool firmware RE
#define CMD_STOP            0x5253      // "RS" - Stop/Brake
#define CMD_JOG             0x4A46      // "JF" - Jog Forward/Reverse
#define CMD_START           0x5354      // "ST" - Start motor
#define CMD_SET_SPEED       0x5356      // "SV" - Set/Read speed
#define CMD_GET_FLAGS       0x4746      // "GF" - Get flags/status
#define CMD_SET_KP          0x4B50      // "KP" - Set speed Kp
#define CMD_SET_KI          0x4B49      // "KI" - Set speed Ki
#define CMD_SET_VKP         0x5650      // "VP" - Set voltage Kp
#define CMD_SET_VKI         0x5649      // "VI" - Set voltage Ki
#define CMD_SET_IR_GAIN     0x4955      // "IU" - Set IR gain (I=0x49, U=0x55) - sends "IU"
#define CMD_SET_IR_OFFSET   0x4F56      // "OV" - Set IR offset (O=0x4F, V=0x56) - sends "OV"
#define CMD_SET_ILIM        0x494C      // "IL" - Set current limit factory max (I=0x49, L=0x4C)
#define CMD_CURRENT_LIMIT   0x434C      // "CL" - Set power output runtime (C=0x43, L=0x4C) - Low/Med/High
#define CMD_SET_BRAKE       0x4252      // "BR" - Set brake mode (B=0x42, R=0x52) - sends "BR"
#define CMD_SET_PULSE_MAX   0x5055      // "PU" - Set pulse max (P=0x50, U=0x55) - sends "PU"
#define CMD_SET_ADV_MAX     0x5341      // "SA" - Set advance max (S=0x53, A=0x41) - sends "SA"
#define CMD_SET_SPD_RMP     0x5352      // "SR" - Set speed ramp
#define CMD_SET_TRQ_RMP     0x5452      // "TR" - Set torque ramp

// PID parameters (discovered 2026-01-25 via service menu)
// NOTE: SP/SI are the REAL Kprop/Kint! VP/VI may be unused or different.
#define CMD_KPROP           0x5350      // "SP" - Kprop (proportional gain, 100%=1000)
#define CMD_KINT            0x5349      // "SI" - Kint (integral gain, 50%=500)
// WARNING: CMD_SAVE_PARAMS was incorrectly defined as 0x5350 (SP) - SP is Kprop!
// There is NO "save params" command - EEPROM save uses RS=1 flag + power cycle

// New commands discovered via logic analyzer (2026-01-22, 2026-01-24)
#define CMD_CURRENT_VELOCITY 0x4356     // "CV" - Current actual RPM (feedback from motor)
#define CMD_KEEP_RUNNING    0x4B52      // "KR" - Heartbeat/watchdog (0=stopped, 9-30=running)
#define CMD_SPEED_2         0x5332      // "S2" - Secondary speed parameter (always 900 in original FW)
#define CMD_GET_VERSION     0x4756      // "GV" - Get MCB firmware version (returns e.g. "B1.7")

// Profile commands (motor behavior during acceleration/load)
// NOTE: Counter-intuitive mapping! S0=HIGH torque, S8=LOW torque (tested 2026-01-14)
#define CMD_PROFILE_S0      0x5330      // "S0" - HARD profile (aggressive, HIGH torque)
#define CMD_PROFILE_S7      0x5337      // "S7" - NORMAL profile (balanced)
#define CMD_PROFILE_S8      0x5338      // "S8" - SOFT profile (gentle, LOW torque)

// Sensor monitoring commands (require CL query unlock at boot!)
#define CMD_HT              0x4854      // "HT" - Heat/thermal query (MCB heatsink temp)
#define CMD_TH              0x5448      // "TH" - Thermal high threshold
#define CMD_TL              0x544C      // "TL" - Thermal low threshold
#define CMD_T0              0x5430      // "T0" - Thermal baseline
#define CMD_LD              0x4C44      // "LD" - Load threshold (unused in original firmware)
#define CMD_LP              0x4C50      // "LP" - LOAD PERCENTAGE query (THE REAL ONE!)
#define CMD_HP              0x4850      // "HP" - Hardware/alert query
#define CMD_SP_QUERY        0x5350      // "SP" - Speed percentage query

// Spindle Hold commands (discovered 2026-01-24 via logic analyzer capture)
#define CMD_V8              0x5638      // "V8" - Voltage param (264 in hold mode)
#define CMD_VR              0x5652      // "VR" - Voltage Ramp (0=off, 100=full) [VALIDATED]
#define CMD_VS              0x5653      // "VS" - Voltage Set/Enable (0=off, 1=on) [VALIDATED]
#define CMD_VG              0x5647      // "VG" - Voltage Gain (261 in hold mode)
#define CMD_SL              0x534C      // "SL" - Speed Limit (10 in hold mode)

// Read commands (query format) - based on SCAN results from MCB
#define CMD_GET_PULSE_MAX   0x5055      // "PU" - Get pulse max (P=0x50, U=0x55) - verified working
#define CMD_GET_ADV_MAX     0x5341      // "SA" - Get advance max (S=0x53, A=0x41) - verified working
#define CMD_GET_IR_GAIN     0x4955      // "IU" - Get IR gain (I=0x49, U=0x55) - verified working
#define CMD_GET_IR_OFFSET   0x4F56      // "OV" - Get IR offset (O=0x4F, V=0x56) - verified working
#define CMD_GET_CUR_LIM     0x494C      // "IL" - Get current limit (I=0x49, L=0x4C) - verified working
#define CMD_GET_SPD_RMP     0x5352      // "SR" - Get speed ramp (S=0x53, R=0x52) - verified working
#define CMD_GET_TRQ_RMP     0x5452      // "TR" - Get torque ramp (T=0x54, R=0x52) - verified working

// ============================================================================
// UNDOCUMENTED COMMANDS (discovered 2026-01-24 via disassembly analysis)
// Found in motor control area 0x801a000-0x801b200, purpose inferred from context
// Commands marked [VALIDATED] were confirmed via service mode logic analyzer captures
// ============================================================================

// Additional Speed Profiles (S0-S9, only S0/S2/S7/S8 documented above)
#define CMD_PROFILE_S1      0x5331      // "S1" - Speed profile 1
#define CMD_PROFILE_S3      0x5333      // "S3" - Speed profile 3
#define CMD_PROFILE_S4      0x5334      // "S4" - Speed profile 4
#define CMD_PROFILE_S5      0x5335      // "S5" - Speed profile 5
#define CMD_PROFILE_S6      0x5336      // "S6" - Speed profile 6
#define CMD_PROFILE_S9      0x5339      // "S9" - Speed profile 9

// Brake commands [VALIDATED in service mode captures]
#define CMD_BF              0x4246      // "BF" - Brake Forward? (3 refs) [VALIDATED]
#define CMD_BN              0x424E      // "BN" - Brake Normal? (3 refs) [VALIDATED]
#define CMD_GR              0x4752      // "GR" - Grip/Brake status query [VALIDATED]

// Motor control
#define CMD_MR              0x4D52      // "MR" - Motor Reset/Ready query
#define CMD_MA              0x4D41      // "MA" - Motor Angle / Max Advance (query at 0x801a984)
#define CMD_F0              0x4630      // "F0" - Fault query (returns 15=no fault, 0-14/50-56=fault)
#define CMD_FD              0x4644      // "FD" - Fault Detect (query + command)
#define CMD_NC              0x4E43      // "NC" - Normal Check? (3 refs)

// F0 fault codes (confirmed from MCB firmware disassembly 2026-03-03)
// Motor is a Switched Reluctance Motor (SRM) with Rotor Position Sensor (RPS) and PFC stage.
// F0=15 is the MCB idle default (no active fault). Other codes only appear when GF bit14 set.
#define MCB_FAULT_NONE      15  // No active fault — MCB idle/default response
#define MCB_FAULT_UNEXPECTED 0  // Unexpected fault / Control Board Issue
#define MCB_FAULT_SRM_STALL  1  // SRM Not Rotate — check motor connection/drill freedom
#define MCB_FAULT_RPS_ERR0   2  // Rotor Position Sensor error 0 — check RPS wiring
#define MCB_FAULT_RPS_ERR1   3  // Rotor Position Sensor error 1
#define MCB_FAULT_HARDWARE   4  // Hardware fault
#define MCB_FAULT_UNEXPECTED2 5 // Unexpected error
#define MCB_FAULT_UVL       13  // Low voltage (UVL) — power-down; resets after recovery
#define MCB_FAULT_PFC       14  // PFC fault (NOT motor lock) — Power Factor Correction stage
#define MCB_FAULT_OVERHEAT  50  // Inverter overheated
#define MCB_FAULT_EEPROM_DATA 55 // EEPROM data fault
#define MCB_FAULT_EEPROM    56  // EEPROM error

// Sensor Alignment / Rotor Position Test (from Teknatool FAQ 2017-01-17)
#define CMD_PW              0x5057      // "PW" - Pulse Width (PulseW in menu, 40% for test)
#define CMD_PH              0x5048      // "PH" - Phase selection (A/B/C for Hall sensors)
#define CMD_RP              0x5250      // "RP" - Rotor Position / RPS condition (read-only)
// Note: CL (Current Limit) already defined above - set to 20% for sensor test

// Current/IR extended parameters
#define CMD_CA              0x4341      // "CA" - Current Actual?
#define CMD_CU              0x4355      // "CU" - Current ?
#define CMD_I0              0x4930      // "I0" - IR/Current param 0 (query + command)
#define CMD_I3              0x4933      // "I3" - IR/Current param 3 (query + command)
#define CMD_IH              0x4948      // "IH" - Current High threshold?

// EEPROM commands
#define CMD_EE              0x4545      // "EE" - EEPROM Execute/Enable
#define CMD_EU              0x4555      // "EU" - EEPROM ?
#define CMD_EV              0x4556      // "EV" - EEPROM Version?

// High/Low threshold pairs (Hx/Lx) - limit/threshold settings
#define CMD_HA              0x4841      // "HA" - Advance High
#define CMD_LA              0x4C41      // "LA" - Advance Low
#define CMD_HD              0x4844      // "HD" - Duty High (related to LD)
#define CMD_HF              0x4846      // "HF" - Frequency High
#define CMD_LF              0x4C46      // "LF" - Frequency Low
#define CMD_HI              0x4849      // "HI" - Current(I) High
#define CMD_LI              0x4C49      // "LI" - Current(I) Low
#define CMD_HL              0x484C      // "HL" - Limit High (2 refs)
#define CMD_LL              0x4C4C      // "LL" - Limit Low (2 refs)
#define CMD_HM              0x484D      // "HM" - Motor High
#define CMD_LM              0x4C4D      // "LM" - Motor Low
#define CMD_HN              0x484E      // "HN" - ? High
#define CMD_LN              0x4C4E      // "LN" - ? Low
#define CMD_HO              0x484F      // "HO" - ? High
#define CMD_LO              0x4C4F      // "LO" - ? Low
#define CMD_HR              0x4852      // "HR" - Ramp High
#define CMD_LR              0x4C52      // "LR" - Ramp Low
#define CMD_LT              0x4C54      // "LT" - Temperature Low (complement to HT)
#define CMD_HU              0x4855      // "HU" - ? High
#define CMD_LU              0x4C55      // "LU" - ? Low
#define CMD_HV              0x4856      // "HV" - Voltage High
#define CMD_LV              0x4C56      // "LV" - Voltage Low

// Speed extended commands
#define CMD_SC              0x5343      // "SC" - Speed Control?
#define CMD_SE              0x5345      // "SE" - Set Enable (commit parameter changes) [VALIDATED 2026-01-25]
#define CMD_SI              0x5349      // "SI" - Speed Initial? (3 refs)
#define CMD_SU              0x5355      // "SU" - Speed ? (3 refs)
#define CMD_SX              0x5358      // "SX" - Speed ?

// Temperature extended
#define CMD_TC              0x5443      // "TC" - Temperature Calibration?
#define CMD_TS              0x5453      // "TS" - Temperature Sensor? (3 refs)

// Under-voltage/Utility commands
#define CMD_UD              0x5544      // "UD" - Under-voltage Detect
#define CMD_UH              0x5548      // "UH" - Under-voltage High
#define CMD_UL              0x554C      // "UL" - Under-voltage Low
#define CMD_UV              0x5556      // "UV" - Under-voltage Value (3 refs)
#define CMD_UW              0x5557      // "UW" - Under-voltage Warning (5 refs) [VALIDATED]

// Voltage extended
#define CMD_V0              0x5630      // "V0" - Voltage param 0
#define CMD_V1              0x5631      // "V1" - Voltage param 1

// Warning commands [VALIDATED in service mode captures]
#define CMD_WH              0x5748      // "WH" - Warning High threshold [VALIDATED]
#define CMD_WL              0x574C      // "WL" - Warning Low threshold [VALIDATED]

// Motor direction parameters for CMD_JOG (JF command)
#define DIR_FORWARD         0x6AA       // Parameter for forward (1706 decimal)
#define DIR_REVERSE         0x6AB       // Parameter for reverse (1707 decimal)

// JF Jog/Positioning parameters (discovered 2026-01-24 via disassembly at 0x801a504)
// These appear to be for controlled small movements (sensor alignment?)
// Usage: JF=JOG_START, wait for GF bit 3 clear, RS=0, JF=JOG_END
// NOTE: No callers found in firmware - may be dead code or service mode only
#define JOG_START           0xE56       // Enter jog/positioning mode (3670 decimal)
#define JOG_END             0xE55       // Exit jog/positioning mode (3669 decimal)
#define GF_JOG_BUSY         0x08        // GF bit 3 = jog/movement in progress

// Motor state parameters (discovered via logic analyzer 2026-01-22, updated 2026-01-24)
#define GF_MOTOR_STOPPED        32      // GF response when motor stopped (forward)
#define GF_MOTOR_RUNNING        34      // GF response when motor running (forward)
#define GF_MOTOR_STOPPED_REV    436     // GF response when motor stopped (reverse)
#define GF_MOTOR_RUNNING_REV    438     // GF response when motor running (reverse)
#define CL_IDLE_PERCENT         70      // CL value when motor idle (70%)
#define CL_RUNNING_PERCENT      100     // CL value when motor running (100%)
#define S2_DEFAULT_RPM          900     // S2 default value (always 900 in original FW)
#define KR_STOPPED              0       // KR parameter when stopped
#define KR_STARTUP              100     // KR parameter during brief startup phase
// Note: KR baseline varies with speed - learned dynamically in tapping mode

// CV (Current Velocity) overshoot detection (discovered 2026-01-25)
// Through-hole detection: CV overshoots when tap exits material
#define CV_OVERSHOOT_PERCENT    130     // 130% of target = through-hole exit detected
#define CV_BURST_QUERIES        3       // Number of rapid CV queries before depth decision
#define CV_BURST_INTERVAL_MS    50      // Interval between CV burst queries

// Motor timing and delay constants (Phase 4.1: Named magic numbers)
// Busy-wait delay loops (approximate timing, clock-dependent)
#define MOTOR_UART_SPIN_DELAY_LOOPS     30000   // ~3-5ms at 120MHz - MCB processing delay
#define MOTOR_UART_TX_TIMEOUT_LOOPS     100000  // ~10ms at 120MHz - TX timeout in critical section

// Motor factory default parameters (from Teknatool service manual)
#define MOTOR_FACTORY_PULSE_MAX         185     // PulseMax factory default
#define MOTOR_FACTORY_IR_GAIN           28835   // IR Gain factory default
#define MOTOR_FACTORY_IR_OFFSET         82      // IR Offset factory default
#define MOTOR_FACTORY_ADV_MAX           85      // AdvMax factory default
#define MOTOR_FACTORY_CUR_LIM           70      // Current Limit factory default (%)
#define MOTOR_FACTORY_SPD_RMP           1000    // Speed Ramp factory default
#define MOTOR_FACTORY_TRQ_RMP           2000    // Torque Ramp factory default
#define MOTOR_FACTORY_VOLTAGE_KP        2000    // Voltage Kp factory default
#define MOTOR_FACTORY_VOLTAGE_KI        9000    // Voltage Ki factory default

// Spindle Hold parameters (discovered 2026-01-24 via logic analyzer capture)
#define HOLD_V8_PARAM           264     // V8 voltage param during hold
#define HOLD_VG_PARAM           261     // VG voltage gain during hold
#define HOLD_VR_OFF             0       // VR when hold disabled
#define HOLD_VR_FULL            100     // VR when hold enabled (100% ramp)
#define HOLD_VS_OFF             0       // VS when hold disabled
#define HOLD_VS_ON              1       // VS when hold enabled
#define HOLD_CL_PERCENT         10      // CL during manual hold (10%)
#define HOLD_CL_SAFETY          12      // CL during safety hold (12%) - E-Stop/Guard
#define HOLD_SL_VALUE           10      // SL speed limit during hold
#define HOLD_MAINTAIN_MS        460     // Hold cycle repeat interval (ms)
#define SAFETY_HOLD_TIMEOUT_MS  2000    // Safety hold auto-release timeout (2s, original=5s)

/*===========================================================================*/
/* GPIO - Buttons and Inputs                                                  */
/*===========================================================================*/

// Guard switch (used as foot pedal in tapping mode)
#define GUARD_PIN           GPIO_PIN_2
#define GUARD_PORT          GPIOC
#define GUARD_ACTIVE_HIGH   1           // High = guard open (pedal pressed)

// Start/Stop button
#define START_STOP_PIN      GPIO_PIN_15
#define START_STOP_PORT     GPIOA

// Function buttons (active low)
#define BTN_F1_PIN          GPIO_PIN_10
#define BTN_F1_PORT         GPIOC
#define BTN_F2_PIN          GPIO_PIN_11
#define BTN_F2_PORT         GPIOC
#define BTN_F3_PIN          GPIO_PIN_12
#define BTN_F3_PORT         GPIOC
#define BTN_F4_PIN          GPIO_PIN_2
#define BTN_F4_PORT         GPIOD

// Rotary encoder (PC13/PC14 quadrature, PC15 button, 4 counts/detent)
#define ENC_A_PIN           GPIO_PIN_13
#define ENC_A_PORT          GPIOC
#define ENC_B_PIN           GPIO_PIN_14
#define ENC_B_PORT          GPIOC
#define ENC_BTN_PIN         GPIO_PIN_15
#define ENC_BTN_PORT        GPIOC
#define ENC_COUNTS_PER_DETENT 4

/*===========================================================================*/
/* LCD Display (HX8347 or similar)                                            */
/*===========================================================================*/

#define LCD_SPI             SPI2
#define LCD_CS_PIN          GPIO_PIN_12
#define LCD_CS_PORT         GPIOB
#define LCD_DC_PIN          GPIO_PIN_11  // Data/Command
#define LCD_DC_PORT         GPIOB
#define LCD_RST_PIN         GPIO_PIN_10
#define LCD_RST_PORT        GPIOB
#define LCD_BL_PIN          GPIO_PIN_1   // Backlight
#define LCD_BL_PORT         GPIOB

#define LCD_WIDTH           128
#define LCD_HEIGHT          64      // ST7565/UC1701 monochrome

/*===========================================================================*/
/* Depth Sensor (ADC on PC1)                                                  */
/*===========================================================================*/

// NOTE: Depth/quill position is read via ADC, NOT from MCB!
// PC1 = ADC1 Channel 11, connected to depth potentiometer
// Original firmware uses DMA to transfer ADC readings to SRAM 0x2000006C
// Reference: Ghidra analysis of FUN_08005214 (ADC init function)

#define DEPTH_ADC           ADC1
#define DEPTH_ADC_CHANNEL   11          // ADC1_IN11 = PC1
#define DEPTH_ADC_PORT      GPIOC
#define DEPTH_ADC_PIN       GPIO_PIN_1
#define DEPTH_ADC_SAMPLE    6           // Sample time (71.5 cycles)

// ADC register addresses (GD32F303 / STM32F103 compatible)
// Note: ADC1_BASE is already defined in CMSIS, use raw address
#define DEPTH_ADC1_SR       (*(volatile uint32_t*)0x40012400)
#define DEPTH_ADC1_CR1      (*(volatile uint32_t*)0x40012404)
#define DEPTH_ADC1_CR2      (*(volatile uint32_t*)0x40012408)
#define DEPTH_ADC1_SMPR1    (*(volatile uint32_t*)0x4001240C)
#define DEPTH_ADC1_SQR3     (*(volatile uint32_t*)0x40012434)
#define DEPTH_ADC1_DR       (*(volatile uint32_t*)0x4001244C)

// Depth calibration
// ADC range is 0-4095 (12-bit), maps to full quill travel
// Approximate: ~100mm travel, so ~41 counts per mm
#define DEPTH_COUNTS_PER_MM 41          // Approximate, needs calibration
#define DEPTH_ADC_MIN       0           // Quill fully retracted
#define DEPTH_ADC_MAX       4095        // Quill fully extended

/*===========================================================================*/
/* Buzzer / Sound                                                             */
/*===========================================================================*/

// Buzzer pin - PA8 verified from hardware testing
#define BUZZER_PIN          GPIO_PIN_8
#define BUZZER_PORT         GPIOA
#define BUZZER_TIM          TIM1
#define BUZZER_TIM_CHANNEL  TIM_CHANNEL_1
#define BUZZER_ENABLED      1           // Set to 0 to disable buzzer

/*===========================================================================*/
/* Motor Hardware Enable (Emergency Stop Safety)                            */
/*===========================================================================*/

// Motor enable pin - directly controls motor controller power
// Active HIGH = motor enabled, LOW = motor disabled (hardware cutoff)
// CRITICAL SAFETY: Set LOW immediately on E-Stop for hardware-level safety
#define MOTOR_ENABLE_PIN    GPIO_PIN_4
#define MOTOR_ENABLE_PORT   GPIOD

// Tone frequencies (Hz)
#define TONE_CLICK          4000        // Button click
#define TONE_ERROR          500         // Error beep
#define TONE_SUCCESS        2000        // Success/confirm
#define TONE_STARTUP        1000        // Startup beep

// Tone durations (ms)
#define BEEP_SHORT          30
#define BEEP_MEDIUM         100
#define BEEP_LONG           300

/*===========================================================================*/
/* Firmware Version                                                           */
/*===========================================================================*/

#define FW_VERSION_MAJOR    0
#define FW_VERSION_MINOR    1
#define FW_VERSION_PATCH    0
#define FW_VERSION_STRING   "v0.1.0-RTOS"
#define FW_BUILD_TYPE       "Debug"     // "Custom", "Debug", "Release"

/*===========================================================================*/
/* Motor Power Level Configuration (discovered 2026-01-25)                    */
/*===========================================================================*/

// Motor power output levels - maps to CL (current limit) command
// Based on UI setting → CL mapping from logic analyzer captures
typedef enum {
    MOTOR_POWER_LOW  = 20,   // 20% - Light materials, may stall at low RPM!
    MOTOR_POWER_MED  = 50,   // 50% - General drilling
    MOTOR_POWER_HIGH = 70,   // 70% - Heavy-duty (factory default)
    MOTOR_POWER_MAX  = 100   // 100% - Full torque
} motor_power_t;

/*===========================================================================*/
/* Tapping Trigger Configuration                                              */
/*===========================================================================*/

// Tapping completion actions (universal for all triggers)
typedef enum {
    COMPLETION_STOP = 0,           // Stop in place (motor off)
    COMPLETION_REVERSE_OUT = 1,    // Reverse back to top position
    COMPLETION_REVERSE_TIMED = 2   // Reverse for specified time then stop
} completion_action_t;

// Legacy depth action (mapped to completion_action_t)
typedef enum {
    TAP_DEPTH_ACTION_STOP = 0,      // Stop motor at target depth
    TAP_DEPTH_ACTION_REVERSE = 1    // Reverse motor at target depth (to top)
} tap_depth_action_t;

// Quill mode pedal override behavior (renamed from SMART → QUILL)
typedef enum {
    QUILL_PEDAL_OFF = 0,        // No pedal override (quill direction only)
    QUILL_PEDAL_REVERSE = 1,    // Pedal triggers reverse during cutting only
    QUILL_PEDAL_TOGGLE = 2      // Pedal toggles direction (both cutting and reversing)
} quill_pedal_mode_t;

// Pedal action modes
typedef enum {
    PEDAL_ACTION_HOLD = 0,       // Press=reverse, hold=keep reversing, release=stop
    PEDAL_ACTION_CHIP_BREAK = 1  // Press=timed reverse, auto-resume forward
} pedal_action_t;

// Clutch slip actions
typedef enum {
    CLUTCH_ACTION_REVERSE = 0,   // Immediately reverse (treat as overload)
    CLUTCH_ACTION_ALERT = 1,     // Show warning, keep running
    CLUTCH_ACTION_CONTINUE = 2   // Ignore (clutch working as designed)
} clutch_action_t;

// Runtime tapping settings structure (used in tapping.c)
typedef struct {
    // Trigger enables (combinable)
    uint8_t depth_trigger_enabled;     // Enable depth-based trigger
    uint8_t load_increase_enabled;     // Enable KR spike detection (blind holes)
    uint8_t load_slip_enabled;         // Enable CV overshoot detection (through holes)
    uint8_t clutch_slip_enabled;       // Enable load plateau detection (torque limiter)
    uint8_t quill_trigger_enabled;     // Enable quill direction auto-reverse
    uint8_t peck_trigger_enabled;      // Enable timed peck cycles
    uint8_t pedal_enabled;             // Enable pedal override

    // General settings
    uint16_t speed_rpm;                // Tapping speed (50-500 RPM)

    // Depth trigger settings
    uint8_t depth_action;              // tap_depth_action_t: STOP or REVERSE (legacy)
    uint8_t depth_completion_action;   // completion_action_t: completion behavior

    // Quill trigger settings (renamed from SMART)
    uint8_t quill_pedal_mode;          // quill_pedal_mode_t: OFF/REVERSE/TOGGLE
    uint8_t quill_completion_action;   // completion_action_t: when cycle ends

    // Load increase settings (KR spike - blind holes)
    uint8_t load_increase_threshold;   // % above baseline to trigger (default 60)
    uint16_t load_increase_reverse_ms; // Duration of reversal (default 200ms)
    uint8_t load_completion_action;    // completion_action_t: after reversal

    // Load slip settings (CV overshoot - through holes)
    uint16_t load_slip_cv_percent;     // CV overshoot threshold % (default 130)
    uint8_t load_slip_completion_action; // completion_action_t: after reversal

    // Clutch slip settings (load plateau - torque limiter)
    uint16_t clutch_plateau_ms;        // Time at plateau to trigger (default 500ms)
    uint8_t clutch_action;             // clutch_action_t: action when detected

    // Peck trigger settings (time-based pulses)
    uint16_t peck_fwd_ms;              // Forward pulse duration (ms)
    uint16_t peck_rev_ms;              // Reverse pulse duration (ms)
    uint8_t peck_cycles;               // Number of cycles (0=infinite until depth)
    uint8_t peck_depth_stop;           // 0=complete all cycles, 1=stop at target depth
    uint8_t peck_completion_action;    // completion_action_t: STOP or REVERSE_OUT
    uint16_t peck_reverse_out_ms;      // Reverse duration if REVERSE_TIMED (ms)

    // Pedal settings
    uint8_t pedal_action;              // pedal_action_t: HOLD or CHIP_BREAK
    uint16_t pedal_chip_break_ms;      // Chip break duration if CHIP_BREAK mode (ms)
} tapping_settings_t;

// Default values for tapping triggers
#define TAP_DEFAULT_LOAD_INCREASE_THRESHOLD  60      // 60% KR increase triggers reverse
#define TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS 200     // 200ms reverse time for load spike
#define TAP_DEFAULT_LOAD_SLIP_CV_PERCENT     130     // 130% CV overshoot (through-hole exit)
#define TAP_DEFAULT_CLUTCH_PLATEAU_MS        500     // 500ms at plateau triggers clutch detection
#define TAP_DEFAULT_PECK_FWD_MS              150     // 150ms forward pulse
#define TAP_DEFAULT_PECK_REV_MS              100     // 100ms reverse pulse
#define TAP_DEFAULT_PECK_CYCLES              7       // 7 peck cycles
#define TAP_DEFAULT_PEDAL_CHIP_BREAK_MS      200     // 200ms chip break duration
#define TAP_MIN_CHIP_BREAK_MS                100     // Minimum chip break (> brake delay)
#define TAP_MAX_CHIP_BREAK_MS                2000    // Maximum chip break
#define TAP_DEFAULT_BRAKE_DELAY              100     // 100ms delay between stop and direction change

// Timing parameters (ms)
#define TAP_STOP_DELAY_MS       100     // Delay between stop and reverse
#define TAP_DEBOUNCE_MS         20      // Button debounce time
#define TAP_DEPTH_DEADBAND_MM   20      // 2.0mm deadband for float chuck
#define TAP_HYSTERESIS_MM       5       // 0.5mm hysteresis (prevents oscillation)
#define TAP_TRANSITION_MS       100     // Pause between direction changes
#define TAP_MAX_CYCLE_TIME_MS   30000   // 30s timeout per peck cycle (safety)

/*===========================================================================*/
/* System Timing Constants (M1)                                               */
/*===========================================================================*/

// Motor communication timing
#define MOTOR_RESPONSE_TIMEOUT_MS   250     // Wait time for motor UART response (increased from 100ms for reliability)
#define MOTOR_STATUS_POLL_MS        100     // Motor status query interval (matches ~100ms original firmware timing)

// Phase 10: Adaptive polling rates for optimization
#define MOTOR_STATUS_POLL_IDLE_MS   500     // 2Hz when motor idle (reduced CPU/UART)
#define MOTOR_STATUS_POLL_RUNNING_MS 50     // 20Hz when motor running (better responsiveness)
#define MOTOR_MCB_WRITE_DELAY_MS    100     // Delay after MCB EEPROM write

// Task timing
#define DEPTH_UPDATE_INTERVAL_MS    20      // Depth sensor polling (50 Hz)
#define UI_DISPLAY_INTERVAL_MS      33      // Display update rate (30 Hz)
#define EVENT_QUEUE_TIMEOUT_MS      10      // Main event loop timeout
#define MOTOR_CMD_QUEUE_TIMEOUT_MS  10      // 10ms for responsive peck mode

// UI/Menu delays
#define DEBOUNCE_MS                 200     // Button debounce
#define POLL_LOOP_DELAY_MS          10      // Busy-wait loop delay
#define MESSAGE_DISPLAY_MS          1000    // Status message display time
#define ERROR_DISPLAY_MS            3000    // Error message display time
#define ESTOP_DISPLAY_MS            30000   // E-Stop error display time
#define SPLASH_DISPLAY_MS           2000    // Splash screen display time
#define CONFIRM_DELAY_MS            500     // Confirmation delay

// Brief pauses
#define BRIEF_PAUSE_MS              50      // Short pause between operations

/*===========================================================================*/
/* Buffer Sizes (M1)                                                          */
/*===========================================================================*/

#define MOTOR_UART_BUFFER_SIZE      32      // Motor UART RX/TX buffer size
#define SERIAL_CMD_BUFFER_SIZE      64      // Serial command input buffer

// Code polish: Compile-time assertions for safety
_Static_assert(MOTOR_UART_BUFFER_SIZE >= 32, "UART buffer too small for protocol packets");
_Static_assert(SERIAL_CMD_BUFFER_SIZE >= 32, "Command buffer too small");

/*===========================================================================*/
/* Speed Limits                                                               */
/*===========================================================================*/

#define SPEED_MIN_RPM       50
#define SPEED_MAX_RPM       5500        // CG variant (EU/AUS/NZ) max speed
#define SPEED_DEFAULT_RPM   500
#define SPEED_TAP_DEFAULT   200         // Default tapping speed

/*===========================================================================*/
/* I2C EEPROM Settings Storage                                                */
/*===========================================================================*/

// I2C1 peripheral for EEPROM
#define EEPROM_I2C              I2C1
#define EEPROM_I2C_SPEED        100000      // 100kHz standard mode
#define EEPROM_SCL_PIN          GPIO_PIN_6
#define EEPROM_SDA_PIN          GPIO_PIN_7
#define EEPROM_I2C_PORT         GPIOB

// Flash fallback (if EEPROM not present)
#define SETTINGS_FLASH_ADDR     0x0801F800  // Last 2KB page
#define SETTINGS_MAGIC          0x4E4F5641  // "NOVA"

// EEPROM storage layout
#define EEPROM_SETTINGS_ADDR    0x0000      // Settings start at beginning
#define EEPROM_SETTINGS_SIZE    512         // Max settings size

#endif /* CONFIG_H */

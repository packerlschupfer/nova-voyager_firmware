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

// Bound on the HSE (external 8 MHz crystal) startup wait in clock_init().
// Loop iterations, not milliseconds: this spins on HSI at 8 MHz before any
// timebase exists.
//
// This MUST outlast the bootloader's equivalent wait, and the two error
// directions are not symmetric:
//
//   bootloader gives up first, we keep waiting -> benign. It falls back to HSI
//     and jumps to us; our wait is still running when a slow crystal starts,
//     and we run at full speed. We recover what it gave up on.
//   we give up first, bootloader waits it out  -> bad. A board that refuses to
//     turn its spindle because of a crystal that demonstrably works.
//
// So being over-patient costs nothing on a working board, while being
// impatient costs a machine. nova-voyager_bootloader uses RCC_TIMEOUT = 200000
// iterations, ~190 ms at 8 MHz. We deliberately target well over 2x that in
// wall time rather than matching the count: the two loops are compiled at
// different optimisation levels, so equal iterations are NOT equal time, and
// the ordering must not be able to invert on a compiler change.
//
// 0x60000 = 393216 iterations. Our loop uses a volatile counter (~11 cycles),
// giving ~540 ms; even if the compiler tightened it to the bootloader's ~7
// cycles it is still ~344 ms, comfortably clear of 190 ms either way.
// A healthy 8 MHz crystal starts in 1-10 ms; high-ESR parts up to ~100 ms.
#define HSE_STARTUP_TIMEOUT_LOOPS   0x60000u

// Test-only: env:hse_fail_test defines this to 1 to force the crystal-dead
// path (see src/init.c). Never set in a shipping build — a firmware built with
// it will always refuse to run the motor. Defaulted here so the #if is well
// defined everywhere and production binaries are bit-identical without it.
#ifndef HSE_FAIL_TEST
#define HSE_FAIL_TEST       0
#endif

// Disable the Cortex-M write buffer (ACTLR.DISDEFWBUF) so data bus errors are
// reported PRECISELY — the faulting instruction is the one that raised it, and
// BFAR holds the offending address.
//
// Both 2026-08-29 lockups reported IMPRECISERR with BFARVALID clear, i.e. "a
// data access was refused, somewhere, at some point". That is close to
// undiagnosable. With this set the same event names its own address.
//
// Cost: stores are no longer buffered, so tight store sequences run slower.
// On this workload — a 120 MHz core driving a bit-banged LCD and a 9600 baud
// console — that is not measurable, and the diagnostic value is high on a
// machine that has already locked up twice for unknown reasons. Set to 0 to
// restore buffering if a future workload ever makes it matter.
#ifndef ENABLE_PRECISE_BUS_FAULTS
#define ENABLE_PRECISE_BUS_FAULTS   1
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

/* Console UART (USART1 -> CH340 -> host). Clock-independent, so it lives
 * OUTSIDE the USE_120MHZ split above — it was briefly inside it, which left
 * every 72 MHz env failing to compile. uart_init() derives the divisor from
 * whichever clock actually came up.
 *
 * Not a hardware limit: the CH340 is good for 2 Mbaud. Raised from 9600 on
 * 2026-08-31 because console round-trip time was the bottleneck in on-target
 * automated testing.
 *
 * NOT the motor link: that is USART3 in motor_uart.c and is pinned at 9600 by
 * the MCB's own protocol. Do not "unify" the two. */
/* 115200. The one corrupted command that prompted a drop to 57600 turned out
 * NOT to be a baud problem: it was always the FIRST command after opening the
 * port, and the cause was the CH340 toggling DTR/RTS on open and injecting a
 * spurious byte into the firmware's RX. The host-side fix (settle, flush with
 * a bare CR, then talk) lives in scripts/serial_cmd.py and scripts/tap_drive.py.
 *
 * Kept at 115200 only because it was then measured rather than assumed: a
 * console stress run WHILE a tapping cycle was executing — motor UART polling,
 * LCD repaints and critical sections all active, which is when USART1 (NVIC
 * priority 6, masked by FreeRTOS at BASEPRI 5) is most likely to overrun —
 * came back clean. See the overrun fix in USART1_IRQHandler, which also had to
 * land before this rate was safe: it used to discard a valid byte on every
 * overrun.
 *
 * Divisor error: 120 MHz -> 1041 (+0.065%), 72 MHz -> 625 (exact),
 * 8 MHz fallback -> 69 (+0.64%). All well inside 8N1 tolerance. */
#define CONSOLE_BAUD        115200

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
#define CMD_SET_SPD_RMP     0x444E      // "DN" - Speed ramp (confirmed from disassembly, NOT "SR")
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
#define CMD_LD              0x4C44      // "LD" - Load threshold (50% = overload trip point)
#define CMD_LP              0x4C50      // "LP" - Static config (always 10), NOT live load — KR is the live register
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
#define CMD_GR              0x4752      // "GR" - RPS sensor bitmask (A=bit0, B=bit1, C=bit2) [VALIDATED]

// Motor control
/* MR — meaning UNKNOWN, never sent by this firmware.
 *
 * Established 2026-08-30 by searching the whole project history: MR answers a
 * query frame cleanly with the value 0 (`02 31 4D 52 30 03 1D`) and returns
 * that SAME frame stopped and running alike, across all 10 recorded reads. No
 * capture exists under fault, guard-open or E-Stop. Both "Motor Reset" and
 * "Motor Ready" are labels someone typed into a table or a test script — the
 * "motor rdy" text in old scan output is printed by OUR tool, not by the MCB.
 * A ready flag that reads 0 while the motor runs makes that reading doubtful.
 *
 * COLD-BOOT MEASUREMENT 2026-08-30: MR reads 0 from the earliest moment the
 * HMI can ask (35 ms into task_motor on a power-on), and GF already answered 32
 * at that same instant — the MCB finishes booting before our firmware reaches
 * the motor task. So no readiness transition is observable from this side at
 * all. Catching one, if it exists, needs a logic analyser on the wire.
 *
 * The OEM firmware reaches MR through its send_QUERY path (disassembly at
 * 0x801a43c loads r0=0x4D52 and calls the one-argument query helper, unlike the
 * adjacent ST=0 which loads r1 and calls send_command) — and nothing in the
 * stock image calls that wrapper at all. Writing MR has never been attempted. */
#define CMD_MR              0x4D52      // "MR" - meaning unknown; query-only
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
#define CMD_SX              0x5358      // "SX" - Dead/unused (AC Tapping uses standard JF reversal, not SX)

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
// GF on GB1.7 only ever returns 32 (stopped) or 34 (running) — verified
// empirically across 419 samples spanning all motor states including reverse.
// The 436/438 "reverse" values from older docs do not appear on this MCB;
// direction is encoded in GR bit 2 (see docs/MOTOR_PROTOCOL.md). Direction
// in our firmware is sourced from the command-time `direction_forward`
// flag and propagated to g_state.motor_forward at start/reverse — no
// GF-based readback needed.
#define GF_MOTOR_STOPPED        32      // GF response when motor stopped
#define GF_MOTOR_RUNNING        34      // GF response when motor running

/* GF carries more than the two values above, and comparing the whole word for
 * equality is what made the extras read as "unknown".
 *
 *    32 = 0b0000100000   bit5
 *    34 = 0b0000100010   bit5 + bit1
 *   436 = 0b0110110100   bit5        + bits 2,4,7,8
 *   438 = 0b0110110110   bit5 + bit1 + bits 2,4,7,8
 *
 * Read as a bitfield these are consistent: bit 5 is a base/ready flag present
 * in every sample, bit 1 is "motor running", and bits 2,4,7,8 are additional
 * status. Masking with GF_STATE_MASK collapses all four onto exactly
 * GF_MOTOR_STOPPED / GF_MOTOR_RUNNING.
 *
 * Evidence for the extras being transient (2026-08-31, on-target): 438 appears
 * ONLY inside the window where the motor task is draining queued direction-
 * change commands rather than polling, and is logged with the commanded
 * direction still REV shortly after the state machine has gone forward. It
 * never appeared during a steady run in either direction — a sustained reverse
 * reports plain 34, which is also why direction is NOT derivable from GF and
 * stays owned by command-time state. What bits 2,4,7,8 individually mean is
 * still unidentified; they are deliberately masked out rather than guessed at.
 *
 * Error states are separate and keep bit 14 (0x4000), checked before this.
 *
 * GF_KNOWN_BITS is the union of every bit seen in a non-error sample, and
 * masking ALONE is not enough without it: 0x1234 & GF_STATE_MASK == 32, so a
 * spliced or stray frame would classify as a clean "stopped" and be published
 * as truth. That is precisely the hazard the known_good guard exists to stop.
 * A value is trusted only when it sets the base bit AND sets no bit outside
 * this set. */
#define GF_STATE_MASK           0x22    // bit5 (base) + bit1 (running)
#define GF_KNOWN_BITS           0x1B6   // bits 1,2,4,5,7,8 — all bits ever observed
#define GF_DIR_CHANGE_BIT       0x04    // Bit 2: direction change in progress
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
/* Manual (console/menu) hold auto-release.
 *
 * BRAKE-CYCLING FIX 2026-08-30: the manual hold had NO timeout, and
 * spindle_hold_maintain() re-sends VR -> CL -> VS=ON every HOLD_MAINTAIN_MS.
 * On this MCB that re-ACTUATES the brake rather than sustaining it, so a manual
 * hold chattered the brake at ~2 Hz for as long as it was held — observed on
 * the machine during 10 s and 16 s console holds. A safety hold only ever sees
 * ~4 refreshes before its 2 s auto-release, which is why the OEM-derived
 * refresh design never showed this; the unbounded manual hold is the outlier.
 *
 * 30 s, from the Teknatool manual: "The spindle hold function will commence for
 * 30 seconds." Corroborated independently by the BR command notes in
 * docs/COMPLETE_MOTOR_COMMAND_REFERENCE.md, which record the MCB auto-releasing
 * the powered hold after 30 s — two sources, same number, so this matches the
 * machine rather than being a number we picked. It does NOT fix the
 * re-actuation itself — see spindle_hold_maintain(). */
#define MANUAL_HOLD_TIMEOUT_MS  30000

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
/* LCD Display — ST7920 with Chinese font ROM, 8-bit parallel (PA0-PA7, PB0-PB2) */
/* Text mode: 16x4 chars (8x16 HCGROM font, NOT HD44780 5x8).               */
/* Graphics mode: 128x64 pixels (split-half GRAM addressing).                */
/* CGRAM: 4 chars, each 16x16, displayed via 2-byte codes (NOT HD44780 5x8). */
/* Single-byte 0x00-0x0F: factory CGROM icons (not writable).                */
/* Visible DDRAM rows: 0xC0, 0xD0, 0xC8, 0xD8 (NOT 0x80/0x90/0xA0/0xB0).   */
/*===========================================================================*/

#define LCD_WIDTH           128
#define LCD_HEIGHT          64

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
    COMPLETION_REVERSE_TIMED = 2,  // Reverse for specified time then stop
    /* Back off, then KEEP CUTTING — the cycle does not end. Added when the
     * four per-trigger completion actions were wired up, because the machine
     * already had this behaviour and the three completion values cannot
     * express it: the quill trigger's lift/push is interactive (lift reverses,
     * push resumes) and the pedal chip break reverses briefly and carries on.
     * Without a value for "resume", giving those triggers a completion action
     * would have deleted both behaviours. The back-off duration is the
     * trigger's own (load_increase_reverse_ms, pedal_chip_break_ms), or
     * open-ended when it has none.
     *
     * NOT valid for the depth trigger: depth >= target is still true after a
     * short back-off, so it would re-trigger on the next poll and chatter.
     * The depth row offers three options and the loader clamps it to 0..2. */
    COMPLETION_RESUME = 3
} completion_action_t;

/* The legacy two-value tap_depth_action_t is gone — depth_completion_action
 * covers it with a third option. Kept here only as a note for anyone reading
 * an old EEPROM byte: 0 meant STOP and 1 meant REVERSE, which are exactly
 * COMPLETION_STOP and COMPLETION_REVERSE_OUT, which is why the stored byte
 * needed no migration. */

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
    CLUTCH_ACTION_ALERT = 1      // Show warning, keep running
    /* There used to be a third, CLUTCH_ACTION_CONTINUE = 2, "ignore (clutch
     * working as designed)". Removed: ignoring a detection silently is what
     * clutch_slip_enabled = false already does, one level cheaper, so it was a
     * second way to spell "off". It was also unreachable — the setter caps at
     * 1 — and unimplemented, since clutch_action was never read at all. Two
     * meaningful choices remain and the existing cap is correct for them. */
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
    uint8_t depth_completion_action;   // completion_action_t: what to do at target depth

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
#define DC_BUS_LOW_VOLTAGE_THRESHOLD 300    // Warn below ~300V DC bus (normal ~356V)
/* Consecutive sub-threshold readings before the low-bus warning fires.
 *
 * At the 2 Hz idle poll this is ~2 s. It exists because the bus charges from
 * zero at power-on: an EDGE detector either misses a genuinely low bus (the
 * old `prev_voltage = 0`, which made the first low reading not an edge) or
 * fires on every boot during the charge transient (`prev_voltage = threshold`,
 * which is what the operator saw on 2026-08-30). A sustained condition is the
 * thing worth reporting, and it catches both cases without the transient. */
#define DC_BUS_LOW_DEBOUNCE          4

/* How long a refused START stays on the panel. Long enough to read while
 * standing at the machine, short enough not to hide the status screen. */
#define START_REFUSED_DISPLAY_MS     3000

/* How long a CLUTCH_ACTION_ALERT banner stays on the LCD. The cut continues
 * underneath it, so this is a notification, not a stop — long enough to read at
 * the machine, short enough not to hide the live load bar for the rest of the
 * hole. */
#define TAP_ALERT_DISPLAY_MS         3000

/* Escalation for a RESUME trigger that will not clear.
 *
 * COMPLETION_RESUME backs off and keeps cutting, which is right for chip
 * clearing: the back-off drops the load and the condition goes away. If it
 * does NOT go away — a genuinely bound tap — the cycle repeats forever. Proven
 * on target 2026-08-31: with the load held above the threshold, load-increase
 * fired five reverse/resume cycles in ~2 s and would have continued
 * indefinitely. TAP_MAX_CYCLE_TIME_MS cannot catch it because each individual
 * reverse is short and its timer restarts every pass.
 *
 * So: N re-fires of the SAME trigger inside the window and the trigger stops
 * resuming and backs out of the hole instead. Only applies to the automatic
 * load triggers — pedal and quill are operator-driven, and a person pressing
 * the pedal repeatedly means it. */
#define TAP_RESUME_ESCALATE_COUNT    3
#define TAP_RESUME_ESCALATE_WINDOW_MS 3000

/* How long an ALIGN session may hold the windings energized and the MCB poll
 * suspended before it is dropped automatically. See motor_enter_align(). */
#define ALIGN_SESSION_TIMEOUT_MS     60000

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
/* Factory default spindle speed, matching the original Teknatool firmware.
 *
 * Only reached on a settings reset or a failed EEPROM load — in normal use the
 * speed autosave overwrites speed.default_rpm with whatever the operator last
 * dialled in, so this is the "fresh machine" value, not a cap or a preference.
 * Was 500, which is low for general work and, being equal to a favourite slot,
 * made it impossible to tell a defaults-reset from a favourite step when
 * diagnosing. 900 is the OEM value and sits mid-range. */
#define SPEED_DEFAULT_RPM   900
#define SPEED_TAP_DEFAULT   200         // Default tapping speed

/*===========================================================================*/
/* I2C EEPROM Settings Storage                                                */
/*===========================================================================*/

// EEPROM I2C — bit-bang on PC4/PC5 (original Teknatool firmware bus)
// PB6/PB14 path exists but is WRITE-PROTECTED (WP hardwired to VCC)
#define EEPROM_SCL_PIN          GPIO_PIN_4
#define EEPROM_SDA_PIN          GPIO_PIN_5
#define EEPROM_I2C_PORT         GPIOC
// AT24C02: 256 bytes, 8-byte pages, 1-byte addressing
#define EEPROM_SIZE             256
#define EEPROM_PAGE_SIZE        8
#define EEPROM_ADDR_SIZE        1

// Flash fallback (if EEPROM not present)
// AUDIT FIX (MEDIUM, config.h:626 + ldscript.ld:17): SETTINGS_FLASH_ADDR used
// to be 0x0801F800, which is the last 2 KB page of a 128 KB part — but the
// GD32F303RCT6 has 256 KB flash, so the real last page is 0x0803F800. The
// stale value sat mid-flash, unreserved in the linker script; a build
// >~116 KB (games/demo were within striking distance) would have made
// settings_save() erase a live application page. Both moved to the real
// last page, and ldscript.ld shrinks FLASH accordingly.
// CROSS-REPO INVARIANT: this page must stay outside the DFU-writable region
// advertised by nova-voyager_bootloader's DfuSe layout string. The bootloader's
// flash_is_address_valid() has no reason to know this page is special, so a DFU
// write spanning the advertised region would erase the user's saved settings.
// ldscript.ld already carves the page out of the app (242K, ending 0x0803F7FF);
// the bootloader must advertise 121*2Kg to match, not 122*2Kg. Verified
// 2026-08-29: 121*2Kg ends at 0x0803F7FF, exactly the linker's last app byte,
// so matching costs nothing — the linker errors before an image could need it.
//
// Bootloader side: the carve-out is APP_SETTINGS_RESERVE, which APP_FLASH_END
// derives from; its run-tests.sh decodes the layout string and asserts this
// page falls outside the advertised region. Deliberately reversible there,
// since that bootloader is published and another firmware may want this page.
// If this address ever moves, APP_SETTINGS_RESERVE is the other end to change.
#define SETTINGS_FLASH_ADDR     0x0803F800  // Real last 2KB page of 256KB flash
/* REVIEW FIX: changed from 0x4E4F5641 ("NOVA") on 2026-08-30, at the same time
 * the layout version was reset 2 -> 1.
 *
 * Resetting a version DOWNWARD re-uses numbers already burned on incompatible
 * layouts. v0.1.0 deliberately leaves a rejected blob on the chip rather than
 * destroying it, so a pre-release unit still holds a v2/v3 image; two future
 * layout bumps later the firmware would declare version 2 again and accept
 * that stale, differently-laid-out image — magic, version and CRC would all
 * match, because the CRC is computed over the stored bytes and says nothing
 * about how they are interpreted. Garbage PID and threshold values would go
 * straight into live settings.
 *
 * Changing the magic makes every pre-release image unmatchable for good,
 * which is what "start fresh" actually requires. The version counter is then
 * free to begin at 1 and only ever go up from here. */
#define SETTINGS_MAGIC          0x4E4F5631  // "NOV1" — see note above

// EEPROM storage layout
#define EEPROM_SETTINGS_ADDR    0x0000      // Settings start at beginning
#define EEPROM_SETTINGS_SIZE    512         // Max settings size

#endif /* CONFIG_H */

/**
 * @file shared.h
 * @brief Shared Data Structures and Inter-Task Communication
 *
 * Defines queues, mutexes, and shared state for FreeRTOS tasks.
 */

#ifndef SHARED_H
#define SHARED_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/*===========================================================================*/
/* Utility Macros (Phase 3.2: Reduce duplication)                            */
/*===========================================================================*/

/**
 * @brief Delay for specified milliseconds (FreeRTOS task delay wrapper)
 * @param ms Milliseconds to delay
 *
 * Replaces verbose vTaskDelay(pdMS_TO_TICKS(X)) pattern throughout codebase.
 * Uses FreeRTOS tick-based delay - task yields CPU during delay.
 */
static inline void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*===========================================================================*/
/* Boot Type Detection (RCC Reset Flags)                                      */
/*===========================================================================*/

typedef enum {
    BOOT_COLD = 0,      // Power-on reset (full init, show splash)
    BOOT_SOFT,          // Software reset / OFF button (fast boot, skip splash)
    BOOT_WATCHDOG,      // Watchdog reset (show warning)
    BOOT_PIN,           // External NRST pin reset
} boot_type_t;

// Global boot type - set early in main(), read by all tasks
extern boot_type_t g_boot_type;

// Set by clock_init() when the external crystal failed to start and the core
// fell back to HSI. Standalone rather than a g_state field because clock_init()
// runs before g_state is memset. Read by safety_can_start_motor().
extern volatile bool g_clock_fault;

/* Set by PVD_IRQHandler when VDD falls through the brown-out threshold, and
 * latched until reset. See include/brownout.h. Consulted by
 * safety_can_start_motor(). */
extern volatile bool g_brownout_latched;

/*===========================================================================*/
/* Task Priorities                                                            */
/*===========================================================================*/

#define PRIORITY_IDLE       0
#define PRIORITY_UI         2   // Buttons, encoder, display (keep responsive)
#define PRIORITY_DEPTH      2   // ADC polling (50Hz)
#define PRIORITY_MOTOR      4   // Motor serial comms (A1: highest - safety critical)
#define PRIORITY_TAPPING    3   // Pedal/load response (A1: higher than depth/UI)

/*===========================================================================*/
/* Task Stack Sizes                                                           */
/*===========================================================================*/

// C4 fix: Increased stack sizes to prevent overflow
#define STACK_SIZE_UI       256  // 1024 bytes - menu + display + CGRAM
#define STACK_SIZE_DEPTH    96   // 384 bytes - simple ADC polling
#define STACK_SIZE_MOTOR    192  // 768 bytes - MCB param reads
#define STACK_SIZE_TAPPING  160  // 640 bytes - state machine

/*===========================================================================*/
/* Event Types                                                                */
/*===========================================================================*/

typedef enum {
    // Button events
    EVT_BTN_ZERO        = 0x0001,
    EVT_BTN_MENU        = 0x0002,
    EVT_BTN_F1          = 0x0004,
    EVT_BTN_F2          = 0x0008,
    EVT_BTN_F3          = 0x0010,
    EVT_BTN_F4          = 0x0020,
    EVT_BTN_START       = 0x0040,
    EVT_BTN_GUARD       = 0x0080,
    EVT_BTN_ENCODER     = 0x0100,
    EVT_BTN_ESTOP       = 0x0200,
    EVT_BTN_F1_LONG     = 0x8000,   // F1 long-press (favorite speed)
    EVT_BTN_ENC_LONG    = 0x8001,   // Encoder long-press (status screen)

    // Encoder events
    EVT_ENC_CW          = 0x0400,
    EVT_ENC_CCW         = 0x0800,

    // System events
    EVT_MOTOR_FAULT     = 0x1000,
    EVT_JAM_DETECTED    = 0x2000,
    EVT_DEPTH_TARGET    = 0x4000,
    EVT_LOAD_SPIKE      = 0x8005,   // Load spike detected (configurable threshold)
    EVT_OVERHEAT        = 0x8002,   // MCB temperature too high
    EVT_TEMP_WARNING    = 0x8003,   // MCB temperature warning
    EVT_LOW_VOLTAGE     = 0x8004,   // Power supply voltage issue
    EVT_BOOT_COMPLETE   = 0x8006,   // Boot complete - play beep
} event_type_t;

/*===========================================================================*/
/* Command Types (for motor task)                                             */
/*===========================================================================*/

typedef enum {
    CMD_MOTOR_STOP,
    CMD_MOTOR_STOP_FAST,        // Fast stop for tapping (RS=0 only, no sync overhead)
    CMD_MOTOR_BRAKE,            // BR command - active braking (faster stop)
    CMD_MOTOR_FORWARD,
    CMD_MOTOR_REVERSE,
    CMD_MOTOR_TAP_FORWARD,      // Fast forward for tapping (JF + ST only)
    CMD_MOTOR_TAP_REVERSE,      // Fast reverse for tapping (JF + ST only)
    CMD_MOTOR_SET_SPEED,
    CMD_MOTOR_QUERY_STATUS,
    CMD_MOTOR_QUERY_TEMP,
    CMD_MOTOR_SPINDLE_HOLD,     // Start spindle hold (powered position lock)
    CMD_MOTOR_SPINDLE_HOLD_SAFETY, // Safety spindle hold (E-Stop/Guard) - higher current
    CMD_MOTOR_SPINDLE_RELEASE,  // Release spindle hold
    CMD_MOTOR_READ_PARAMS,      // Read all MCB parameters (async via motor task)
    CMD_MOTOR_APPLY_SETTINGS,   // Apply profile + power output before start
} motor_cmd_type_t;

typedef struct {
    motor_cmd_type_t cmd;
    uint16_t param;
} motor_cmd_t;

/*===========================================================================*/
/* Application State                                                          */
/*===========================================================================*/

typedef enum {
    APP_STATE_STARTUP,
    APP_STATE_IDLE,
    APP_STATE_DRILLING,
    APP_STATE_TAPPING,
    APP_STATE_MENU,
    APP_STATE_ERROR
} app_state_t;

/*===========================================================================*/
/* Shared State (protected by mutex)                                          */
/*===========================================================================*/

typedef struct {
    // Current state
    app_state_t state;

    // Speed
    uint16_t current_rpm;
    uint16_t target_rpm;

    // Depth
    int16_t current_depth;      // 0.1mm units
    int16_t target_depth;
    int16_t depth_offset;       // Calibration

    // Motor status
    bool motor_running;
    bool motor_forward;         // true=forward, false=reverse
    bool motor_fault;
    uint16_t motor_load;

    // Tapping
    // tap_mode_t tap_mode;  // REMOVED: Use trigger enables instead
    uint8_t tap_state;          // TAP_STATE_*

    // UI
    uint8_t menu_index;
    bool menu_active;

    // Settings
    uint8_t depth_mode;         // 0=ignore, 1=stop, 2=revert
    uint8_t units;              // 0=mm, 1=inch

    // Flags
    bool guard_closed;
    bool estop_active;
    bool pedal_pressed;         // Foot pedal on PC3 (X11 connector)
    bool speed_fine_mode;       // Encoder button toggles: true=fine, false=coarse
    bool tapping_armed;         // F2 toggles: triggers active only when armed
    uint8_t row3_mode;          // F4 cycles: 0=load bar, 1=temperature, 2=speed info, 3=clean
    bool flash_in_progress;     // Set during flash erase/write — blocks motor start
    uint16_t dc_bus_voltage;    // MCB DC bus voltage from UD register (~356V)
#ifdef BUILD_GAMES
    bool game_mode_active;
#endif
    uint8_t  fault_code;        // MCB F0 fault code (0-15, 50, 55, 56)

    // Simulation mode (for testing without hardware)
    bool sim_mode;              // When true, don't read pedal/depth from hardware
    int16_t sim_depth;          // Simulated depth value
    /* Load/RPM simulation for the three load-based tapping triggers, which
     * cannot otherwise be exercised without cutting real metal. Separate
     * active-flags rather than a sentinel so the zero-initialised state means
     * "off". Debug builds only — the overrides compile out of release.
     * NOTE these feed the jam detectors too, exactly as real readings do: a
     * faked load spike can and should trip a jam. */
    bool sim_load_active;       // When true, motor_load comes from sim_load
    uint8_t sim_load;           // Simulated motor load (%)
    bool sim_cv_active;         // When true, motor_get_actual_rpm() returns sim_cv
    uint16_t sim_cv;            // Simulated actual RPM (for load-slip CV overshoot)

    // Error message display
    uint32_t error_until;       // Show error until this tick (0=no error)
    const char* error_line1;    // Error message line 1
    const char* error_line2;    // Error message line 2

    // Task heartbeat monitoring (watchdog safety)
    uint32_t heartbeat_main;    // Last heartbeat from main task
    uint32_t heartbeat_ui;      // Last heartbeat from UI task
    uint32_t heartbeat_motor;   // Last heartbeat from motor task
    uint32_t heartbeat_depth;   // Last heartbeat from depth task
    uint32_t heartbeat_tapping; // Last heartbeat from tapping task

    // Queue saturation monitoring (diagnostic counters)
    uint16_t event_queue_overflows;  // Count of dropped event queue items
    uint16_t motor_queue_overflows;  // Count of dropped motor queue items

    // MCB parameters (populated by CMD_MOTOR_READ_PARAMS)
    struct {
        int32_t pulse_max;
        int32_t adv_max;
        int32_t ir_gain;
        int32_t ir_offset;
        int32_t cur_lim;
        int32_t spd_rmp;
        int32_t trq_rmp;
        int32_t voltage_kp;
        int32_t voltage_ki;
        bool valid;  // True if successfully read
    } mcb_params;

} shared_state_t;

/*===========================================================================*/
/* Global Handles (defined in main.c)                                         */
/*===========================================================================*/

extern QueueHandle_t g_event_queue;         // UI events → main logic
extern QueueHandle_t g_motor_cmd_queue;     // Commands → motor task
extern SemaphoreHandle_t g_state_mutex;     // Protects shared_state
extern SemaphoreHandle_t g_motor_mutex;     // Serializes motor control sequences
extern SemaphoreHandle_t g_uart_mutex;      // Protects UART console output
extern shared_state_t g_state;              // Shared application state

extern TaskHandle_t g_task_ui;
extern TaskHandle_t g_task_motor;
extern TaskHandle_t g_task_depth;
extern TaskHandle_t g_task_tapping;

/*===========================================================================*/
/* Helper Macros                                                              */
/*===========================================================================*/

/*
 * CRITICAL: MUTEX ORDERING DISCIPLINE
 * ====================================
 * To prevent deadlock, follow these rules for mutex acquisition:
 *
 * PRIMARY RULE: Never hold STATE_LOCK when acquiring MOTOR_CONTROL_LOCK
 * -----------------------------------------------------------------------
 *
 * Safe Pattern 1 (Independent access):
 *   STATE_LOCK();
 *   read/write state variables;
 *   STATE_UNLOCK();
 *   // ... (state lock released)
 *   MOTOR_CONTROL_LOCK();
 *   motor_command();
 *   MOTOR_CONTROL_UNLOCK();
 *
 * Safe Pattern 2 (Atomic motor control sequence - used in tapping):
 *   MOTOR_CONTROL_LOCK();        // Lock motor operations first
 *   STATE_LOCK();                // OK: Take state briefly
 *   read state variables;
 *   STATE_UNLOCK();              // Release state immediately
 *   motor_commands();            // Issue commands with motor locked
 *   MOTOR_CONTROL_UNLOCK();
 *
 * FORBIDDEN Pattern (DEADLOCK RISK):
 *   STATE_LOCK();
 *   MOTOR_CONTROL_LOCK();        // DEADLOCK! Another task may reverse order
 *   ...                          // Never acquire motor while holding state!
 *
 * Rationale:
 *   - Motor lock enables atomic read-decide-command sequences
 *   - State lock held briefly within motor lock is safe (one-way nesting)
 *   - Reverse nesting (state contains motor) causes deadlock
 *   - Pattern 2 prevents race: motor state can't change during decision
 *
 * Verification: All code follows these patterns (verified 2026-01-12)
 */

// Lock state mutex (with timeout)
// DEBUG: Track mutex acquisition for deadlock detection
#ifdef BUILD_DEBUG
    /* REVIEW FIX (HIGH): this used to be a single global bool, which
     * MOTOR_CONTROL_LOCK() then read as "the calling task holds STATE" — it
     * cannot mean that. Any task preempted inside STATE_LOCK() leaves it set,
     * so the check below fired on a completely unrelated task. Record the
     * OWNER instead; then "do I hold it?" is answerable. */
    extern volatile void* g_state_mutex_owner;
    #define STATE_LOCK()    do { \
        xSemaphoreTake(g_state_mutex, portMAX_DELAY); \
        g_state_mutex_owner = (void*)xTaskGetCurrentTaskHandle(); \
    } while(0)
    #define STATE_UNLOCK()  do { \
        g_state_mutex_owner = NULL; \
        xSemaphoreGive(g_state_mutex); \
    } while(0)
#else
    #define STATE_LOCK()    xSemaphoreTake(g_state_mutex, portMAX_DELAY)
    #define STATE_UNLOCK()  xSemaphoreGive(g_state_mutex)
#endif

// Send event to queue (non-blocking from task context)
#define SEND_EVENT(evt) do { \
    event_type_t e = (evt); \
    if (xQueueSend(g_event_queue, &e, 0) != pdTRUE) { \
        taskENTER_CRITICAL(); \
        g_state.event_queue_overflows++; \
        taskEXIT_CRITICAL(); \
    } \
} while(0)

// Send event from ISR
#define SEND_EVENT_ISR(evt) do { \
    BaseType_t xHigherPriorityTaskWoken = pdFALSE; \
    event_type_t e = (evt); \
    if (xQueueSendFromISR(g_event_queue, &e, &xHigherPriorityTaskWoken) != pdTRUE) { \
        g_state.event_queue_overflows++; \
    } \
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken); \
} while(0)

// Motor control mutex - hold during read-decide-command sequences
// Prevents race conditions between tasks controlling the motor
#ifdef BUILD_DEBUG
    extern volatile void* g_state_mutex_owner;
    /* REVIEW FIX (HIGH): two bugs in three lines. It tested a global flag that
     * any task could have set, and it answered a suspected lock-ordering
     * problem with `for(;;)` in whichever task tripped it — routinely
     * task_motor, at priority 4, which starves every other task until the IWDG
     * resets the machine. A debug aid that bricks the board on a false positive
     * is worse than the inversion it looks for. Now: only the task that
     * actually holds STATE trips it, and it reports once and carries on, which
     * is enough to find the offending path in a log. */
    #define MOTOR_CONTROL_LOCK()    do { \
        if (g_state_mutex_owner == (void*)xTaskGetCurrentTaskHandle()) { \
            static bool _warned = false; \
            if (!_warned) { \
                extern void uart_puts(const char*); \
                _warned = true; \
                uart_puts("[MUTEX] ORDER VIOLATION: STATE held while taking MOTOR\r\n"); \
            } \
        } \
        xSemaphoreTakeRecursive(g_motor_mutex, portMAX_DELAY); \
    } while(0)
    #define MOTOR_CONTROL_UNLOCK()  xSemaphoreGiveRecursive(g_motor_mutex)
#else
    #define MOTOR_CONTROL_LOCK()    xSemaphoreTakeRecursive(g_motor_mutex, portMAX_DELAY)
    #define MOTOR_CONTROL_UNLOCK()  xSemaphoreGiveRecursive(g_motor_mutex)
#endif

// Send motor command (with timeout to prevent deadlock - H2 fix)
// Phase 7: Added diagnostics tracking
// Returns pdTRUE on success, pdFALSE on timeout/failure
#define MOTOR_CMD_TIMEOUT_MS    100
#define MOTOR_CMD(c, p) do { \
    motor_cmd_t cmd = {.cmd = (c), .param = (p)}; \
    if (xQueueSend(g_motor_cmd_queue, &cmd, pdMS_TO_TICKS(MOTOR_CMD_TIMEOUT_MS)) != pdTRUE) { \
        /* Queue full - motor task may be stuck, log and trigger fault */ \
        extern void diagnostics_queue_overflow(bool is_motor_queue); \
        taskENTER_CRITICAL(); \
        g_state.motor_queue_overflows++; \
        taskEXIT_CRITICAL(); \
        diagnostics_queue_overflow(true); \
        SEND_EVENT(EVT_MOTOR_FAULT); \
    } \
} while(0)

// Phase 1.2: Critical motor command - emergency stop if queue full
// Use for safety-critical commands (stop, emergency stop, etc.)
#define MOTOR_CMD_SEND_CRITICAL(c, p) do { \
    motor_cmd_t cmd = {.cmd = (c), .param = (p)}; \
    if (xQueueSend(g_motor_cmd_queue, &cmd, pdMS_TO_TICKS(MOTOR_CMD_TIMEOUT_MS)) != pdTRUE) { \
        extern void motor_emergency_stop(void); \
        extern void uart_puts(const char* s); \
        uart_puts("[CRITICAL] Motor cmd queue full - EMERGENCY STOP\r\n"); \
        taskENTER_CRITICAL(); \
        g_state.motor_queue_overflows++; \
        taskEXIT_CRITICAL(); \
        motor_emergency_stop(); \
        SEND_EVENT(EVT_MOTOR_FAULT); \
    } \
} while(0)

/*===========================================================================*/
/* Boot Magic Constants (Phase 2.5: Consolidated from commands.c and main.c) */
/*===========================================================================*/

// Force cold boot magic (stored in RAM, survives warm reset)
// AUDIT FIX (MEDIUM, shared.h:380): moved off the .bss overlap that
// corrupted the g_state_mutex kernel object. Now a NOLOAD-section variable
// (see ldscript.ld ".noinit"), so startup won't zero it before boot
// detection reads the flag.
extern volatile uint32_t g_force_cold_boot_magic;
#define FORCE_COLD_BOOT_MAGIC_ADDR  (&g_force_cold_boot_magic)
#define FORCE_COLD_BOOT_MAGIC_VALUE 0xC01DB007  // "COLD BOOT"

/*===========================================================================*/
/* Task Heartbeat Monitoring (Watchdog Safety)                               */
/*===========================================================================*/

// Watchdog timeout and heartbeat thresholds
#define WATCHDOG_TIMEOUT_MS     3000   // 3 second watchdog (reduced from 10s)
#define HEARTBEAT_TIMEOUT_MS    2000   // Task must heartbeat within 2s

// Update task heartbeat (call periodically from each task)
#define HEARTBEAT_UPDATE_MAIN()     do { g_state.heartbeat_main = HAL_GetTick(); } while(0)
#define HEARTBEAT_UPDATE_UI()       do { g_state.heartbeat_ui = HAL_GetTick(); } while(0)
#define HEARTBEAT_UPDATE_MOTOR()    do { g_state.heartbeat_motor = HAL_GetTick(); } while(0)
#define HEARTBEAT_UPDATE_DEPTH()    do { g_state.heartbeat_depth = HAL_GetTick(); } while(0)
#define HEARTBEAT_UPDATE_TAPPING()  do { g_state.heartbeat_tapping = HAL_GetTick(); } while(0)

// Check if task heartbeat is stale (returns true if task is alive)
#define HEARTBEAT_IS_ALIVE(last_beat) ((HAL_GetTick() - (last_beat)) < HEARTBEAT_TIMEOUT_MS)

/**
 * @brief How long to wait after raising motor_scan_mode before touching the MCB.
 *
 * REVIEW FIX: the envelopes added for MSYNC/MSAVE and the two menu paths used
 * 100 ms. task_motor consults motor_scan_mode only at the top of its loop, so
 * the settle has to cover a WORST-CASE IN-FLIGHT POLL — which
 * commands_motor.c::scan_settle_aborted() already documents as
 * 5 x MOTOR_RESPONSE_TIMEOUT_MS ~= 1.25 s, and covers with 1.5 s. At 100 ms the
 * poll's bytes could still splice the parameter writes for most of a second:
 * exactly the corruption the envelope exists to prevent.
 */
#define MOTOR_SCAN_SETTLE_MS  1500

/**
 * @brief Claim / release the MCB parameter-write envelope (task_motor.c).
 *
 * Always claim through motor_scan_try_claim() rather than writing
 * motor_scan_mode directly: the decision and the claim have to happen inside
 * one STATE_LOCK region, and only the successful claimer may release — the
 * release is ownership-checked, so a release from another task is a no-op.
 * See the definition in task_motor.c for the defects that came from doing this
 * by hand at the call sites.
 */
typedef enum {
    MOTOR_SCAN_CLAIMED = 0,  /**< yours; call motor_scan_release() when done */
    MOTOR_SCAN_BUSY,         /**< machine is mid-job (running / drilling / tapping) */
    MOTOR_SCAN_HELD,         /**< another task already holds the envelope */
} motor_scan_result_t;

motor_scan_result_t motor_scan_try_claim(void);
bool motor_scan_held_by_caller(void);
void motor_scan_release(void);

/**
 * @brief What to tell the operator about a claim result.
 *
 * Returns NULL for a SUCCESSFUL claim rather than a failure string: the
 * previous version folded MOTOR_SCAN_CLAIMED into "already in progress", so
 * `motor_scan_refusal(motor_scan_try_claim())` would have reported a reason for
 * a claim that worked. Every caller checks for success first today; this stops
 * the next one having to.
 */
static inline const char* motor_scan_refusal(motor_scan_result_t r) {
    switch (r) {
        case MOTOR_SCAN_BUSY: return "machine is running — STOP first";
        case MOTOR_SCAN_HELD: return "MCB parameter write already in progress";
        case MOTOR_SCAN_CLAIMED:
        default:              return NULL;
    }
}

/* REVIEW FIX: commands_motor.c settles with a `MOTOR_SCAN_SETTLE_MS / 100`
 * loop of 100 ms steps, which truncates — 1550 would silently settle for 1500,
 * and anything under 100 for not at all — while menu.c honours the value
 * exactly via delay_ms_ui(). Keep them from diverging by construction. */
_Static_assert(MOTOR_SCAN_SETTLE_MS % 100 == 0 && MOTOR_SCAN_SETTLE_MS >= 100,
               "MOTOR_SCAN_SETTLE_MS must be a whole number of 100 ms steps");

/**
 * @brief delay_ms() for long UI-task work, keeping the heartbeat fresh.
 *
 * delay_ms() is vTaskDelay — the task yields, but its heartbeat is only
 * refreshed at the top of task_ui's loop (task_ui.c:432). Menu actions run
 * inside that loop via process_buttons -> menu_click, so an action that blocks
 * longer than HEARTBEAT_TIMEOUT_MS (2000) makes heartbeat_ui go stale.
 *
 * Two consequences, and an earlier version of this comment conflated them:
 *   - IMMEDIATELY at ~2 s, main.c stops refreshing the IWDG and starts
 *     printing "[WATCHDOG] Task failure detected / UI stuck" every loop. That
 *     is a console flood and a false alarm, not a reset.
 *   - A RESET needs the IWDG to actually expire, ~5 s of no refresh on top of
 *     that, so roughly 7 s of contiguous blocking. Reset-defaults' ~2.5 s was
 *     over the alarm threshold but inside the reset margin.
 *
 * Use this for any UI wait of a few hundred ms or more; it beats every 200 ms
 * while it waits.
 */
static inline void delay_ms_ui(uint32_t ms) {
    while (ms > 200u) {
        HEARTBEAT_UPDATE_UI();
        vTaskDelay(pdMS_TO_TICKS(200));
        ms -= 200u;
    }
    HEARTBEAT_UPDATE_UI();
    if (ms) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    HEARTBEAT_UPDATE_UI();
}

#define ALL_TASKS_ALIVE() ( \
    HEARTBEAT_IS_ALIVE(g_state.heartbeat_main) && \
    HEARTBEAT_IS_ALIVE(g_state.heartbeat_ui) && \
    HEARTBEAT_IS_ALIVE(g_state.heartbeat_motor) && \
    HEARTBEAT_IS_ALIVE(g_state.heartbeat_depth) && \
    HEARTBEAT_IS_ALIVE(g_state.heartbeat_tapping) \
)

/*===========================================================================*/
/* Task Entry Points                                                          */
/*===========================================================================*/

void task_ui(void *pvParameters);
void task_motor(void *pvParameters);
void task_depth(void *pvParameters);
void task_tapping(void *pvParameters);

/*===========================================================================*/
/* Initialization                                                             */
/*===========================================================================*/

void shared_init(void);

#endif /* SHARED_H */

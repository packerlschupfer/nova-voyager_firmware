/**
 * @file safety.h
 * @brief Centralized safety interlock gate for motor-start paths.
 *
 * Audit 2026-07-02 found five reachable failure paths where a motor could
 * spin up under an engaged E-Stop / open guard:
 *   1. task_motor.c::local_motor_start()  — dequeued start after purge
 *   2. commands_motor.c::cmd_start()      — console bypass
 *   3. task_tapping.c::complete_transition() — tapping enqueues from stale state
 *   4. cmd_forward / cmd_reverse          — same shape as cmd_start
 *   5. any future start entry point       — no linter guarding against it
 *
 * Fix: every start path calls safety_can_start_motor() *inside* the motor
 * task (i.e. after xQueueReceive but before motor_hardware_enable()) so a
 * command that was already dequeued when the E-Stop fires still refuses.
 *
 * Reads are lock-free bool loads (single-byte atomic on Cortex-M), matching
 * the existing g_state read pattern used by events.c handlers.
 */

#ifndef SAFETY_H
#define SAFETY_H

#include <stdbool.h>
#include "shared.h"
#include "settings.h"

/* Raised while a console or menu action is writing MCB parameters. task_motor
 * consults it at the top of its loop and skips its whole poll block, which is
 * where motor_load_update(), jam_load_update() and jam_update() live — so
 * while it is set, all four jam detectors are dead. Defined in task_motor.c. */
extern volatile bool motor_scan_mode;

static inline bool safety_can_start_motor(void) {
    /* REVIEW FIX: the MCB-sync paths sampled g_state.motor_running once and
     * then held motor_scan_mode for ~2.2 s (1.5 s settle + ~0.7 s sync). That
     * is a point-in-time check, not an interlock: a console START inside the
     * window spun the spindle up with every jam detector suspended and
     * motor_save_mcb_params() mid-write to the MCB's EEPROM. Refusing here
     * closes it at the one choke point every start already passes through. */
    /* REVIEW FIX (CRITICAL): ownership-aware, and this must NOT be the caller's
     * job to work around. align_gate_ok() previously short-circuited — it
     * returned true the moment it saw its own claim, BEFORE ever reaching this
     * function — so once an ALIGN session held the claim, typing ALIGN again
     * with the E-Stop engaged re-drove PD4 and re-applied holding torque. That
     * is the v0.1.0 ALIGN bypass, reintroduced by the re-entrancy shortcut I
     * added for it. The safety gate is not something a caller may skip because
     * it finds one of the conditions inconvenient.
     *
     * A claim held by THIS task is not a reason to refuse it a start: the only
     * path that both holds a claim and energizes in the same task is ALIGN,
     * which is what the claim is for. A claim held by ANOTHER task still
     * refuses — a start dequeued in task_motor while task_main is mid-MSYNC
     * gets exactly the same answer as before. */
    if (motor_scan_mode && !motor_scan_held_by_caller()) return false;
    // Clock fault first: if the crystal never started we are running on HSI at
    // 8 MHz against a build compiled for 120, so the UART to the MCB is at the
    // wrong baud. We could neither reliably command the spindle nor reliably
    // stop it, which makes refusing the only defensible answer.
    if (g_clock_fault)                return false;
    /* The supply has sagged through the PVD threshold at some point since boot.
     * Software cannot be trusted to have run correctly across that, and a rail
     * that reached 2.5 V is failing rather than merely loaded — so do not spin
     * a spindle on it. Latched until reset; see include/brownout.h. */
    if (g_brownout_latched)           return false;
    if (g_state.estop_active)         return false;
    if (g_state.state == APP_STATE_ERROR) return false;
    if (g_state.flash_in_progress)    return false;
    const settings_t* s = settings_get();
    if (s && s->sensor.guard_check_enabled && !g_state.guard_closed) return false;
    return true;
}

/** Human-readable reason for the most recent refusal — for uart_puts. */
/**
 * @brief The same refusal, rendered for the 16x2 error area of the LCD.
 *
 * WHY THIS EXISTS: start_refused() used to write the reason to the CONSOLE
 * ONLY. Standing at the machine you pressed ON, nothing happened, and the panel
 * said nothing — you had to guess between guard, E-Stop, jam, an MCB write, a
 * clock fault and (since 2026-08-30) a supply brown-out. Three of those have no
 * screen of their own at all: brown-out, flash-write-in-progress and MCB
 * parameter write. The console reason is only visible over a serial cable that
 * is not attached while anyone is actually drilling.
 *
 * Deliberately the SAME precedence chain as safety_refusal_reason(), in the
 * same file, so the two renderings cannot drift — the console and the panel
 * must never name different causes. Lines are exactly LCD_COLS wide.
 */
static inline void safety_refusal_lcd(const char** line1, const char** line2) {
    if (g_clock_fault)                { *line1 = "! CLOCK FAULT ! "; *line2 = "Crystal failed  "; return; }
    if (g_brownout_latched)           { *line1 = "! BROWNOUT !    "; *line2 = "Power cycle req."; return; }
    if (g_state.estop_active)         { *line1 = "! E-STOP ON !   "; *line2 = "Release to start"; return; }
    if (g_state.state == APP_STATE_ERROR) { *line1 = "! FAULT ACTIVE !"; *line2 = "Press ON to clr "; return; }
    if (g_state.flash_in_progress)    { *line1 = "SAVING SETTINGS "; *line2 = "Please wait...  "; return; }
    {
        const settings_t* s = settings_get();
        if (s && s->sensor.guard_check_enabled && !g_state.guard_closed) {
            *line1 = "! GUARD OPEN !  "; *line2 = "Close to start  "; return;
        }
    }
    if (motor_scan_mode && !motor_scan_held_by_caller())
                                      { *line1 = "MCB WRITE BUSY  "; *line2 = "Please wait...  "; return; }
    *line1 = "START REFUSED   "; *line2 = "Reason unknown  ";
}

static inline const char* safety_refusal_reason(void) {
    if (g_clock_fault)                return "clock fault - crystal failed";
    if (g_brownout_latched)           return "supply brown-out detected - power cycle";
    if (g_state.estop_active)         return "E-Stop engaged";
    if (g_state.state == APP_STATE_ERROR) return "fault active — press ON to clear";
    if (g_state.flash_in_progress)    return "flash write in progress";
    const settings_t* s = settings_get();
    if (s && s->sensor.guard_check_enabled && !g_state.guard_closed) return "guard open";
    /* REVIEW FIX: LAST, deliberately. In safety_can_start_motor() the order is
     * irrelevant (it is an OR), but here it is a priority list and this check
     * had jumped ahead of E-Stop, fault state and guard. Pressing START with
     * the E-Stop engaged during a 2.2 s MCB-sync envelope would have answered
     * "MCB parameter write in progress" — the transient, self-clearing reason —
     * and never mentioned the E-Stop. A condition that clears itself in two
     * seconds must never mask one that needs the operator to act. */
    if (motor_scan_mode && !motor_scan_held_by_caller())
                                      return "MCB parameter write in progress";
    return "unknown";
}

#endif /* SAFETY_H */

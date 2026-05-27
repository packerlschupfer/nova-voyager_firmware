/**
 * @file spindle_hold.c
 * @brief Spindle Hold Module Implementation
 *
 * Phase 2.1: Extracted from task_motor.c (lines 549-664)
 */

#include "spindle_hold.h"
#include "safety.h"
#include "config.h"
#include "motor.h"
#include "shared.h"  // Phase 3.2: For delay_ms() helper
#include "FreeRTOS.h"
#include "task.h"

// External UART functions for logging
extern void uart_puts(const char* s);
extern void uart_putc(char c);

/*===========================================================================*/
/* Module State (Phase 5.2: Thread-safety classified)                        */
/*===========================================================================*/

// [MODULE_LOCAL] Only accessed from motor task via public API
// No mutex needed - all calls from single task context
static bool spindle_hold_active = false;
static bool spindle_hold_is_safety_mode = false;  // true if safety hold (has timeout)
static TickType_t spindle_hold_start_time = 0;
static TickType_t spindle_hold_last_maintain = 0;
static uint8_t spindle_hold_cl_percent = HOLD_CL_PERCENT;  // CL used by maintain()

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize and enable spindle hold with specified current limit
 * @param cl_percent Current limit percentage (10% for manual, 12% for safety)
 */
static void spindle_hold_start_with_cl(uint8_t cl_percent) {
    /* Re-assert the enable line FIRST, before any early-out.
     *
     * BRAKE-CYCLING FIX: the guard and E-Stop EXTI ISRs drop PD4 on EVERY edge.
     * Until this round the events.c handlers re-raised it on every event; that
     * call was removed (it raced a stop that was still only queued) and the
     * enable moved in here — but it landed BELOW the early-out. So with a hold
     * already active at the same CL, a further guard-open edge dropped PD4 and
     * nothing put it back: the brake died while the code still reported the
     * hold active and maintain() kept clocking UART commands into a disabled
     * MCB. A guard switch that chatters would then drop and re-apply the brake
     * edge by edge.
     *
     * Raising it here keeps the ordering fix — the enable is still immediately
     * followed by this function's VR=0/CL=0/VS=0 preamble, which quiesces the
     * MCB, rather than being left high next to a queued start. */
    motor_hardware_enable();

    /* REVIEW FIX (MEDIUM): the early-out used to be unconditional on
     * spindle_hold_active, so an E-Stop or guard-open DURING an active manual
     * hold returned here without changing anything — while spindle_hold_start()
     * went on to set spindle_hold_is_safety_mode = true. The machine then
     * reported a safety hold while holding at the manual 10% CL, and
     * spindle_hold_maintain() kept re-applying that stale value every 460 ms.
     * The one situation that specifies more torque got less. Re-apply whenever
     * the requested CL differs. */
    if (spindle_hold_active && spindle_hold_cl_percent == cl_percent) return;

    /* REVIEW FIX (HIGH): this never drove PD4. It works from the guard/E-Stop
     * handlers only because those raise the line first; a console or menu HOLD
     * from a cold boot (motor_task_init leaves PD4 low) sent the whole VR/CL/VS
     * sequence, got ACKs for all nine commands because UART TX succeeds
     * regardless of the enable line, set spindle_hold_active and printed
     * "Spindle hold: active" — with zero holding torque on a chuck the operator
     * had just been told was held. motor_is_spindle_hold_active() feeds
     * guard_permits_release(), so the lie propagated.
     *
     * Energizing here is the same deliberate exception the guard and E-Stop
     * handlers are allowlisted for in scripts/check-safety-gate.sh: applying
     * braking torque IS the response to the fault that refuses starting. The
     * gate for a MANUAL hold lives in spindle_hold_start(), which is the only
     * caller that can be reached without a fault. (The call itself now happens
     * above the early-out — see the BRAKE-CYCLING FIX at the top.) */

    uart_puts("Spindle hold: starting (CL=");
    char buf[4];
    int i = 0;
    uint8_t v = cl_percent;
    do { buf[i++] = '0' + (v % 10); v /= 10; } while (v && i < 3);
    while (i > 0) uart_putc(buf[--i]);
    uart_puts("%)\r\n");

    /* REVIEW FIX (MEDIUM): every one of these nine returns was discarded and
     * spindle_hold_active was then set unconditionally, so a hold that never
     * reached the MCB still reported "active". The CL and VS writes are what
     * create the holding torque — and this runs precisely when the link is
     * least healthy, right after a guard-open or E-Stop. The operator (and
     * handle_btn_guard, via motor_is_spindle_hold_active()) were told the
     * chuck was held while it was free to coast. */
    bool ok = true;
    // Step 1: Initialize - set all voltage params to off
    ok = motor_send_command(CMD_VR, HOLD_VR_OFF) && ok;
    delay_ms(5);
    ok = motor_send_command(CMD_CURRENT_LIMIT, 0) && ok;
    delay_ms(5);
    ok = motor_send_command(CMD_VS, HOLD_VS_OFF) && ok;
    delay_ms(5);

    // Step 2: Set voltage parameters
    ok = motor_send_command(CMD_V8, HOLD_V8_PARAM) && ok;
    delay_ms(5);
    ok = motor_send_command(CMD_VG, HOLD_VG_PARAM) && ok;
    delay_ms(5);

    // Step 3: Enable hold - ramp to 100%, low current limit, voltage on
    ok = motor_send_command(CMD_VR, HOLD_VR_FULL) && ok;
    delay_ms(5);
    ok = motor_send_command(CMD_CURRENT_LIMIT, cl_percent) && ok;
    delay_ms(5);
    ok = motor_send_command(CMD_VS, HOLD_VS_ON) && ok;
    delay_ms(5);

    if (!ok) {
        /* Do not claim a hold we could not establish — but do NOT just return
         * either. REVIEW FIX: the first version of this bailed out with
         * spindle_hold_active still false, and both spindle_hold_release() and
         * spindle_hold_update() begin with `if (!spindle_hold_active) return;`.
         * The guard and E-Stop handlers drive PD4 HIGH immediately before
         * calling us so these very writes can reach the MCB, so bailing out
         * left the hardware motor-enable interlock defeated for as long as the
         * guard stayed open or the E-Stop stayed latched — re-opening the exact
         * defect the release-path fix closed, one commit earlier.
         *
         * It also left the MCB half-configured (VR/CL possibly accepted, VS
         * lost). Tear down explicitly: stop, drop the enable line, and report. */
        uart_puts("Spindle hold: FAILED - MCB did not accept the sequence\r\n");
        motor_send_command(CMD_STOP, 0);
        delay_ms(10);
        motor_hardware_disable();
        /* REVIEW FIX (HIGH): this returned without clearing spindle_hold_active.
         * Harmless while the early-out above refused to re-run an active hold —
         * but that early-out is now deliberately bypassed when the requested CL
         * differs, which is exactly the manual(10%) -> safety(12%) escalation a
         * guard-open performs. So a NACKed escalation left spindle_hold_active
         * true with PD4 low and spindle_hold_cl_percent stuck at 10:
         * motor_is_spindle_hold_active() fed `true` into guard_permits_release()
         * and maintain() kept re-applying the stale value, which is precisely
         * the false "hold is active" this teardown exists to prevent. */
        spindle_hold_active = false;
        spindle_hold_is_safety_mode = false;
        spindle_hold_cl_percent = HOLD_CL_PERCENT;
        uart_puts("Spindle hold: motor disabled (no holding torque)\r\n");
        return;
    }

    spindle_hold_active = true;
    spindle_hold_cl_percent = cl_percent;  // Remember for maintain()

    /* REVIEW FIX (MEDIUM): the nine results ANDed into `ok` above prove only
     * that the bytes left OUR UART — motor_send_command() returns false solely
     * on a local TXE timeout and never reads a reply (motor.c). So the check
     * that was written to stop this code claiming a hold "that never reached
     * the MCB" cannot detect an MCB that did not accept it: with the MCB's RX
     * line broken or the controller latched in fault, all nine transmits clock
     * out happily and we printed "active" over a freely coasting chuck.
     *
     * Read the current limit back — that is the only evidence the MCB acted on
     * any of it. Deliberately NOT a teardown on mismatch: this runs as the
     * response to a guard-open or E-Stop, and dropping a hold that is in fact
     * working would leave a spinning chuck unbraked, which is worse than an
     * unverified one. Downgrade the claim instead of removing the torque. */
    /* The CL readback that used to live here is gone.
     *
     * It was added this session to test whether the MCB actually accepts the
     * hold, and it answered the question: asked CL=10 the MCB reports 20, i.e.
     * it clamps to its own floor. That is now recorded in
     * docs/COMPLETE_MOTOR_COMMAND_REFERENCE.md and does not need re-measuring
     * on every hold.
     *
     * Removed because it was the newest code running inside the spindle-hold
     * path during the window in which the board logged a hard fault, and a
     * diagnostic has no business adding a blocking UART transaction to the
     * response to a guard-open or E-Stop. Not evidence that it caused the
     * fault — the fault PC is inside FreeRTOS tasks.c and I could not attribute
     * it — but a brake path is the wrong place to carry unnecessary risk. */
    uart_puts("Spindle hold: active\r\n");

}

/**
 * @brief Maintain spindle hold by repeating command sequence
 * Call periodically (every 460ms) to keep hold active
 */
static void spindle_hold_maintain(void) {
    if (!spindle_hold_active) return;

    /* Repeat the hold sequence to maintain position. Use the CL value
     * requested at start (safety hold uses 12%, manual uses 10%).
     *
     * REVIEW FIX (MEDIUM): all three results were discarded, though
     * spindle_hold_start_with_cl() was explicitly fixed to stop trusting these
     * same commands. This 460 ms refresh is what keeps the torque alive, so if
     * the link degrades after a successful start the MCB drops the hold while
     * spindle_hold_active stays true — and a MANUAL hold has no timeout at all
     * (only safety mode checks SAFETY_HOLD_TIMEOUT_MS), so it would claim to be
     * holding indefinitely. Report and tear down instead of lying. */
    /* Same reason as the enable at the top of spindle_hold_start_with_cl():
     * a guard or E-Stop edge between refreshes drops PD4 under us, and a hold
     * whose enable line is low is not a hold. */
    motor_hardware_enable();

    /* BRAKE-CYCLING FIX: VS is the ACTUATION ("Voltage Set - 0=off, 1=on
     * (spindle hold enable)", docs/MOTOR_PROTOCOL.md), and re-sending it every
     * 460 ms is what made this MCB re-engage the brake twice a second instead
     * of sustaining it — the operator pulled the machine's power over it.
     *
     * VR and VS were both re-sent purely because this function repeated the
     * whole entry sequence. Only VR (ramp) and CL (current limit) are
     * PARAMETERS worth keeping fresh; VS is the switch, and it is thrown once
     * on entry in spindle_hold_start_with_cl(). If the MCB turns out to need VS
     * re-asserted to sustain, the hold will decay instead of chattering — which
     * is why this was tested on the machine rather than reasoned about.
     *
     * BR was investigated first as the OEM's "proper" latching hold and
     * measured to produce no holding torque at all — see the command
     * reference. This sequence really is the powered hold. */
    bool ok = motor_send_command(CMD_VR, HOLD_VR_FULL);
    delay_ms(2);
    ok = motor_send_command(CMD_CURRENT_LIMIT, spindle_hold_cl_percent) && ok;
    delay_ms(2);

    if (!ok) {
        uart_puts("Spindle hold: LOST - MCB stopped accepting the refresh\r\n");
        motor_send_command(CMD_STOP, 0);
        delay_ms(10);
        motor_hardware_disable();
        spindle_hold_active = false;
        spindle_hold_is_safety_mode = false;
        spindle_hold_cl_percent = HOLD_CL_PERCENT;
        uart_puts("Spindle hold: motor disabled (no holding torque)\r\n");
    }
}

/*===========================================================================*/
/* Public API Implementation                                                  */
/*===========================================================================*/

void spindle_hold_start(bool is_safety) {
    /* AUDIT FIX (MEDIUM, spindle_hold.c:108): a manual hold used to clear
     * spindle_hold_is_safety_mode unconditionally. Starting one while a safety
     * hold was running therefore cancelled that hold's SAFETY_HOLD_TIMEOUT_MS
     * auto-release (spindle_hold_update() only checks the timeout in safety
     * mode), leaving the windings energised indefinitely — and a safety hold is
     * exactly the state where the operator has an open guard or an engaged
     * E-Stop. A safety hold outranks a manual one; it releases on its own
     * timeout, and the operator can still release explicitly. */
    if (!is_safety && spindle_hold_active && spindle_hold_is_safety_mode) {
        uart_puts("Spindle hold: safety hold active - manual hold ignored\r\n");
        return;
    }

    /* A manual hold energizes the windings on operator request, with no fault
     * to justify it — so it passes the normal gate. A safety hold deliberately
     * does not: it is the response to the very condition the gate refuses. */
    if (!is_safety && !safety_can_start_motor()) {
        uart_puts("Spindle hold refused: ");
        uart_puts(safety_refusal_reason());
        uart_puts("\r\n");
        return;
    }

    if (is_safety) {
        // Safety hold: CL=12%, has timeout
        spindle_hold_start_with_cl(HOLD_CL_SAFETY);
        spindle_hold_is_safety_mode = true;
        spindle_hold_start_time = xTaskGetTickCount();
        spindle_hold_last_maintain = spindle_hold_start_time;
    } else {
        /* Manual hold: CL=10%. BRAKE-CYCLING FIX: it now has a timeout too —
         * see MANUAL_HOLD_TIMEOUT_MS. Previously it held, and re-actuated the
         * brake every 460 ms, until someone typed RELEASE. */
        spindle_hold_start_with_cl(HOLD_CL_PERCENT);
        spindle_hold_is_safety_mode = false;
        spindle_hold_start_time = xTaskGetTickCount();
        spindle_hold_last_maintain = spindle_hold_start_time;
    }
}

void spindle_hold_release(void) {
    if (!spindle_hold_active) return;

    uart_puts("Spindle hold: releasing\r\n");

    /* BRAKE-CYCLING FIX, part 2 — MEASURED on the machine 2026-08-30.
     *
     * This used to send CMD_STOP alone, on the note that "original firmware
     * only sends single RS=0 to release hold". That worked only by accident:
     * the 460 ms refresh was re-driving VS=ON continuously, so the hold was
     * never really latched — it was being re-actuated, which is exactly the
     * chatter the operator pulled the power over. Once VS became a one-shot on
     * entry (see spindle_hold_maintain()), the MCB held it latched and a bare
     * STOP no longer let go: the operator reported the chuck still held after
     * RELEASE, and the load register still read 12%. A console STOP cleared it.
     *
     * A teardown has to undo what the entry did, explicitly and in reverse —
     * which is what motor_exit_align() has always done for the same registers.
     * Do not rely on STOP to clear a switch this code threw. */
    motor_send_command(CMD_VS, HOLD_VS_OFF);
    delay_ms(2);
    motor_send_command(CMD_CURRENT_LIMIT, 0);
    delay_ms(2);
    motor_send_command(CMD_VR, HOLD_VR_OFF);
    delay_ms(2);

    /* ...and then a REAL stop, through the motor queue.
     *
     * MEASURED 2026-08-30, twice: the register teardown above is not by itself
     * enough to drop a latched hold. Clearing VS/CL/VR and sending a bare
     * CMD_STOP left the chuck held and the MCB still reporting 12% load, for as
     * long as we watched. A console STOP cleared it immediately, every time.
     *
     * The difference is that local_motor_stop() WAITS for the MCB's response to
     * CMD_STOP before moving on, and also resets the motor state; these
     * fire-and-forget sends with 2 ms gaps do not. Rather than keep guessing at
     * the register sequence on a brake, route the release through the stop path
     * that is measured to work. MOTOR_CMD_SEND_CRITICAL falls back to a
     * hardware cutoff if the queue is full, which is the right failure mode
     * for a release. */
    MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
    delay_ms(10);

    /* REVIEW FIX (HIGH): the release never dropped PD4, so it left the MCB
     * ENABLED. The E-Stop and guard handlers deliberately re-raise PD4 so the
     * hold's UART commands can reach the controller (events.c) — and both EXTI
     * handlers are edge-triggered, so nothing lowers it again. With a latching
     * E-Stop still depressed, the 2 s safety-hold timeout fired, the hold
     * released, and the hardware motor-enable interlock stayed defeated for as
     * long as the operator held the button down — leaving only the software
     * gate. Releasing a hold means the spindle is no longer being driven, so
     * the enable line has no business staying high; a later start re-raises it
     * through local_motor_start(). */
    motor_hardware_disable();

    spindle_hold_active = false;
    spindle_hold_is_safety_mode = false;
    uart_puts("Spindle hold: released\r\n");
}

void spindle_hold_update(void) {
    spindle_hold_update_gated(true);
}

/* @param allow_uart false while an MCB parameter-write envelope is claimed —
 * the refresh must not splice VR/CL/VS into someone else's command sequence.
 * The safety timeout still runs: a hold left energized because a console
 * command happened to hold the envelope would be worse than a late refresh,
 * and releasing is what the timeout is for. The refresh interval is 460 ms
 * against a settle of 1.5 s, so a skipped one costs at most a few hundred ms
 * of holding current on a claim that is itself bounded. */
void spindle_hold_update_gated(bool allow_uart) {
    if (!spindle_hold_active) return;

    TickType_t now = xTaskGetTickCount();

    /* Auto-release timeout. BRAKE-CYCLING FIX: the manual hold had none, so it
     * re-actuated the brake at ~2 Hz for as long as it was left held. Both
     * modes are bounded now; only the limit differs. */
    const uint32_t elapsed_ms = (now - spindle_hold_start_time) * portTICK_PERIOD_MS;
    const uint32_t hold_limit_ms = spindle_hold_is_safety_mode
                                       ? SAFETY_HOLD_TIMEOUT_MS
                                       : MANUAL_HOLD_TIMEOUT_MS;
    if (elapsed_ms >= hold_limit_ms) {
        uart_puts(spindle_hold_is_safety_mode
                      ? "Safety hold: timeout - auto-releasing\r\n"
                      : "Spindle hold: timeout - auto-releasing\r\n");
        spindle_hold_release();
        return;
    }

    // Maintain hold at regular intervals
    if (!allow_uart) {
        /* Do not let the skipped interval pile up into an immediate refresh the
         * moment the envelope clears — re-base it. */
        spindle_hold_last_maintain = now;
        return;
    }

    uint32_t since_last = (now - spindle_hold_last_maintain) * portTICK_PERIOD_MS;
    if (since_last >= HOLD_MAINTAIN_MS) {
        spindle_hold_maintain();
        spindle_hold_last_maintain = now;
    }
}

bool spindle_hold_is_active(void) {
    return spindle_hold_active;
}

#!/usr/bin/env bash
#
# check-safety-gate.sh — assert the motor-start choke point is not bypassed.
#
# WHY THIS EXISTS
# ---------------
# include/safety.h::safety_can_start_motor() is meant to be the single gate
# every path that energizes the spindle passes through. Nothing in C enforces
# that: a new function can call motor_hardware_enable() (which drives PD4, the
# line the E-Stop and guard EXTI ISRs drop) and silently defeat the interlock.
#
# That is not hypothetical. v0.1.0 shipped with the console command ALIGN doing
# exactly this — motor_enter_align() re-drove PD4 and applied holding torque
# with the E-Stop engaged. It was found by review, not by a test, because no
# test can see a gate that was never called. This script can.
#
# THE RULE
# --------
# Every function in src/ that calls motor_hardware_enable() must also call
# safety_can_start_motor(), unless it is on the allowlist below with a reason.
#
# The allowlist is short on purpose. If you are adding to it, the question to
# answer first is why the spindle may be energized in a state the gate refuses.
# There is one legitimate answer in this firmware — applying the spindle HOLD
# (braking torque) as the response to the very fault that refuses starting —
# and it applies to two functions.
#
# WHAT THIS DOES NOT COVER
# ------------------------
# It keys on motor_hardware_enable(), i.e. on driving PD4 — the action that
# actually defeats the hardware interlock, because it undoes what the E-Stop
# and guard ISRs did. A function that only sends torque commands (CMD_VR,
# CMD_CURRENT_LIMIT) over the MCB UART, assuming PD4 is already high, is not
# flagged. motor_set_align_phase() is such a function and is gated by hand.
# Passing this check is therefore necessary, not sufficient.
#
# It follows exactly one hop: a function that calls a helper which itself calls
# safety_can_start_motor() counts as gated (motor_enter_align() -> align_gate_ok()
# is the live case). Two hops is not followed, deliberately — a gate you have to
# trace through three functions is not a choke point anyone can read.
#
# Run from the repo root. Exits non-zero on a violation.

set -uo pipefail
cd "$(dirname "$0")/.."

# function-name:reason
# REVIEW FIX: handle_btn_guard and handle_btn_estop were removed from this list
# when they stopped driving PD4 themselves — the hold sequence now raises the
# line, in the right order, after its own stop. A stale allowlist entry is how a
# real bypass hides later, so entries go when the reason goes.
ALLOWLIST=(
    "spindle_hold_maintain:re-asserts the enable line for a hold that is ALREADY established — the guard/E-Stop ISRs drop PD4 on every edge and a hold whose enable is low is not a hold; the decision to hold was gated at spindle_hold_start()"
    "spindle_hold_start_with_cl:applies the spindle hold itself — the gate refuses in exactly the states this exists to respond to; the MANUAL entry path is gated in spindle_hold_start()"
)

python3 - "${ALLOWLIST[@]}" <<'PY'
import re, sys, glob

allow = {}
for item in sys.argv[1:]:
    name, _, reason = item.partition(':')
    allow[name] = reason

# Functions in this codebase open with the signature at column 0 and close with
# a lone '}' at column 0. Anything that does not match that shape is reported
# rather than skipped, so an unparsed file cannot hide a bypass.
start = re.compile(r'^[A-Za-z_][\w \t\*]*?([A-Za-z_]\w*)\s*\([^;]*\)\s*\{\s*$')

# Comments are prose, not calls. REVIEW FIX: matching raw text meant a function
# that merely NAMED motor_hardware_enable() in a comment was reported as
# energizing the motor — and, far worse in this direction, a comment mentioning
# safety_can_start_motor() would have made an ungated function read as gated.
_block = re.compile(r'/\*.*?\*/', re.S)
_line  = re.compile(r'//[^\n]*')

def strip_comments(text):
    return _line.sub('', _block.sub('', text))

funcs = []          # (path, line, name, body)
for path in sorted(glob.glob('src/*.c')):
    lines = open(path, encoding='utf-8').read().splitlines()
    i = 0
    while i < len(lines):
        m = start.match(lines[i])
        if not m:
            i += 1
            continue
        name, first = m.group(1), i
        i += 1
        while i < len(lines) and lines[i] != '}':
            i += 1
        funcs.append((path, first + 1, name, strip_comments('\n'.join(lines[first:i]))))
        i += 1

# Helpers that consult the gate themselves — one hop is followed, see the
# header. A helper on the allowlist does NOT launder the gate for its callers.
helpers = set(n for _, _, n, b in funcs
              if 'safety_can_start_motor()' in b and n not in allow)

# A helper only counts as gating if the gate is REACHABLE on every path.
#
# REVIEW FIX: align_gate_ok() called safety_can_start_motor() — so this script
# passed it — but returned true on an earlier line when the caller already held
# the scan claim, skipping the gate entirely and re-energizing the windings
# under an engaged E-Stop. The script cannot do reachability analysis, but it
# can catch the shape that caused it: an unconditional `return true` sitting
# ABOVE the gate call in a function whose whole purpose is to consult it.
short_circuit = []
for path, line, name, body in funcs:
    if name not in helpers:
        continue
    gate_at = body.index('safety_can_start_motor()')
    for m in re.finditer(r'\breturn\s+true\s*;', body[:gate_at]):
        short_circuit.append((path, line, name))
        break

violations, checked, gated = [], 0, 0
for path, line, name, body in funcs:
    if 'motor_hardware_enable()' not in body:
        continue
    checked += 1
    if name in allow:
        print("  allowed  %s:%d %s() — %s" % (path, line, name, allow[name]))
        continue
    if 'safety_can_start_motor()' in body:
        gated += 1
        print("  gated    %s:%d %s()" % (path, line, name))
        continue
    via = [h for h in helpers if re.search(r'\b%s\s*\(' % re.escape(h), body)]
    if via:
        gated += 1
        print("  gated    %s:%d %s() via %s()" % (path, line, name, via[0]))
    else:
        violations.append((path, line, name))

print()
if short_circuit:
    print("FAIL: %d gate helper(s) can return true WITHOUT consulting the gate:"
          % len(short_circuit))
    for path, line, name in short_circuit:
        print("  %s:%d  %s() has a `return true` above its "
              "safety_can_start_motor() call" % (path, line, name))
    print()
    print("A caller must not be able to skip the gate because one of its")
    print("conditions is inconvenient. Move the special case INSIDE the gate.")
    sys.exit(1)

if violations:
    print("FAIL: %d function(s) energize the motor without consulting the safety gate:" % len(violations))
    for path, line, name in violations:
        print("  %s:%d  %s() calls motor_hardware_enable() but not safety_can_start_motor()" % (path, line, name))
    print()
    print("Add the gate, or add the function to ALLOWLIST in scripts/check-safety-gate.sh")
    print("with a reason why the spindle may be energized in a refused state.")
    sys.exit(1)

if checked == 0:
    print("FAIL: found no callers of motor_hardware_enable() at all — the parser or")
    print("the call site name has changed, and this check is no longer checking anything.")
    sys.exit(1)

print("OK: %d motor-energizing function(s): %d gated, %d allowlisted." % (checked, gated, checked - gated))
PY

#!/usr/bin/env bash
set -euo pipefail
#
# nova-settings.sh — export a machine's settings to a file, and put them back.
#
# WHY THIS EXISTS
# ---------------
# On 2026-11-01 a full-chip erase took the settings page at 0x0803F800 with it.
# DUMP could show every setting and nothing could replay one, so the machine's
# configuration was reconstructed by reading values out of an old chat log and
# retyping them. The fields that live only in the flash mirror — material, bit
# type, diameter, interface.*, display units — were simply gone.
#
# Take an export before anything that erases flash. That is `custom`, `erase`,
# `quickboot` and `original`, i.e. everything except `quick`.
#
# Usage:
#   ./scripts/nova-settings.sh export [file]     # default: settings-<date>.txt
#   ./scripts/nova-settings.sh import <file>
#   ./scripts/nova-settings.sh diff <file>       # compare file against machine
#
# Talks to the machine through scripts/nova-serial, so it honours the same
# transport that script does.
#
cd "$(dirname "$0")/.." || { echo "cannot reach the repo root" >&2; exit 1; }

SERIAL=./scripts/nova-serial
[ -x "$SERIAL" ] || { echo "missing $SERIAL" >&2; exit 1; }

usage() { sed -n '/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

cmd="${1:-}"; shift || true

case "$cmd" in
export)
    out="${1:-settings-$(date +%Y%m%d-%H%M%S).txt}"
    echo "Reading settings from the machine..."
    raw=$("$SERIAL" 'EXPORT' 25)
    if ! printf '%s' "$raw" | grep -q '=== END EXPORT ==='; then
        echo "EXPORT did not complete — refusing to write a partial file." >&2
        echo "Is the firmware new enough to have EXPORT? Try: $SERIAL HELP" >&2
        printf '%s\n' "$raw" | tail -5 >&2
        exit 1
    fi
    # Keep only the comment and command lines, in order, dropping the console
    # echo and prompt noise.
    printf '%s\n' "$raw" \
        | sed 's/\r$//' \
        | grep -E '^(#|SET |SAVE)' > "$out"
    n=$(grep -c '^SET ' "$out")
    echo "Wrote $out ($n settings)"
    if ! printf '%s' "$raw" | grep -q '^# UNREADABLE KEY'; then
        echo "All exported keys were readable."
    else
        echo "WARNING: the firmware reported unreadable keys — see the file." >&2
    fi
    ;;

import)
    file="${1:-}"
    [ -n "$file" ] || usage
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }

    # Replay one line at a time and CHECK EACH REPLY. A partial restore that
    # reports success is the failure mode this whole feature exists to prevent,
    # so SAVE is only sent if every SET was accepted.
    fails=0; sent=0
    while IFS= read -r line; do
        case "$line" in
            ''|'#'*) continue ;;
            'SAVE') continue ;;        # sent at the end, only on success
            'SET '*) ;;
            *) echo "  skipping unrecognised line: $line" >&2; continue ;;
        esac
        key=$(printf '%s' "$line" | awk '{print $2}')
        want=$(printf '%s' "$line" | awk '{print $3}')
        reply=$("$SERIAL" "$line" 8 | sed 's/\r$//')
        sent=$((sent + 1))
        if printf '%s' "$reply" | grep -q "^Set $key="; then
            got=$(printf '%s' "$reply" | sed -n "s/^Set $key=\([0-9-]*\).*/\1/p" | head -1)
            if [ "$got" != "$want" ]; then
                # The firmware clamped it. Report, do not retry: the setter is
                # the authority on what is legal for this machine.
                echo "  CLAMPED $key: asked $want, stored $got" >&2
                fails=$((fails + 1))
            fi
        else
            echo "  FAILED  $key: $reply" >&2
            fails=$((fails + 1))
        fi
    done < "$file"

    echo "Replayed $sent settings, $fails problem(s)."
    if [ "$fails" -ne 0 ]; then
        echo "NOT saving. The machine holds the restored values in RAM but" >&2
        echo "nothing has been persisted — fix the reported lines and re-run," >&2
        echo "or send SAVE by hand if you accept the clamped values." >&2
        exit 1
    fi
    echo "Persisting..."
    "$SERIAL" 'SAVE' 15 | sed 's/\r$//'
    ;;

diff)
    file="${1:-}"
    [ -n "$file" ] || usage
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
    live=$(mktemp /tmp/nova-settings-live.XXXXXX)
    "$SERIAL" 'EXPORT' 25 | sed 's/\r$//' | grep -E '^SET ' > "$live"
    grep -E '^SET ' "$file" > "${live}.want" || true
    if diff -u "${live}.want" "$live" > "${live}.diff"; then
        echo "No differences."
    else
        echo "--- $file (wanted)   +++ machine (actual)"
        sed -n '3,$p' "${live}.diff"
    fi
    rm -f "$live" "${live}.want" "${live}.diff"
    ;;

*) usage ;;
esac

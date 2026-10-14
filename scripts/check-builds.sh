#!/usr/bin/env bash
set -euo pipefail
#
# check-builds.sh — build every environment CI builds, locally.
#
# WHY THIS EXISTS
# ---------------
# On 2026-10-13 commit c1e624e added a 1280-byte jam flight recorder and was
# tested with `pio run -e release`, which passed. CI builds four envs, and two
# of them — nova_voyager_games and nova_voyager_demo — carry the game task's
# state plus a 4 KB FreeRTOS heap, so they sit a few hundred bytes below the
# DFU hand-off word at 0x20004FF0. The new buffer pushed them over it and
# ldscript.ld's ASSERT turned that into a link error.
#
# CI went red on that commit and stayed red for EIGHT commits, because every
# local check that was run — `pio run -e release`, `pio test -e native`, and
# the three gate scripts — passed the whole time. A green `-e release` says
# nothing about the RAM-tight builds.
#
# Run this before pushing. It mirrors .github/workflows/ci.yml's build step.
#
set -uo pipefail
cd "$(dirname "$0")/.." || { echo "cannot reach the repo root" >&2; exit 1; }

# Keep in step with .github/workflows/ci.yml
ENVS=(nova_voyager nova_voyager_games nova_voyager_demo debug)

fail=0
for env in "${ENVS[@]}"; do
    printf '%-22s ' "$env"
    if out=$(pio run -e "$env" 2>&1); then
        # Report the RAM figure: these builds fail on RAM, not flash, and a
        # number creeping up is the warning before the link error.
        ram=$(printf '%s\n' "$out" | grep -oE 'RAM: *\[[^]]*\] *[0-9.]+%[^(]*\([^)]*\)' | tail -1)
        echo "OK   ${ram:-}"
    else
        echo "FAILED"
        printf '%s\n' "$out" | grep -E 'error|ASSERT|overlaps|reached the DFU' | head -5 | sed 's/^/    /'
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo
    echo "At least one environment failed to build. CI builds all of these;"
    echo "a passing 'pio run -e release' does not cover them."
    exit 1
fi

echo
echo "All ${#ENVS[@]} CI environments build."

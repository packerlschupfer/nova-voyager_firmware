#!/bin/bash
# Assert the build is actually pinned — not merely configured to look pinned.
#
# Why this exists: `platform_packages` declared in [common] is SILENTLY IGNORED
# unless every env references it. That happened here on 2026-08-29: the config
# named framework-stm32cubef1@1.8.6, CI happily installed 1.8.7, and the release
# binary differed from the hardware-validated one by 4 bytes. Nothing warned.
#
# So don't check the config file — check what PlatformIO actually RESOLVED.
# A pin is only real if `pio pkg list` reports it as "required: ... @ <exact>".
set -uo pipefail

ENV="${1:-nova_voyager}"
CORE_EXPECTED="6.1.19"

# package -> exact version it must resolve to. Keep in step with platformio.ini.
declare -A EXPECTED=(
  ["toolchain-gccarmnoneeabi"]="1.70201.0"
  ["framework-stm32cubef1"]="1.8.7"
  ["tool-ldscripts-ststm32"]="0.2.0"
)

fail=0

core=$(pio --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ "$core" != "$CORE_EXPECTED" ]; then
    echo "FAIL  PlatformIO core is $core, expected $CORE_EXPECTED"
    echo "      (workflows must pin: pip install platformio==$CORE_EXPECTED)"
    fail=1
else
    echo "ok    PlatformIO core $core"
fi

listing=$(pio pkg list -e "$ENV" 2>/dev/null)
for pkg in "${!EXPECTED[@]}"; do
    wanted="${EXPECTED[$pkg]}"
    line=$(printf '%s\n' "$listing" | grep -F "$pkg @" | head -1)
    if [ -z "$line" ]; then
        echo "FAIL  $pkg not found in env '$ENV'"
        fail=1
        continue
    fi
    # first occurrence only: the greedy form picks up the one inside "(required: ...)"
    resolved=$(printf '%s' "$line" | grep -oP "\Q$pkg\E @ \K[^ )]+" | head -1)
    # "required: ... @ <exact>" means pinned; a range means the pin is not applied
    if ! printf '%s' "$line" | grep -qF "required: platformio/$pkg @ $wanted"; then
        echo "FAIL  $pkg resolved $resolved but is NOT pinned to $wanted"
        echo "      $line"
        echo "      Does every env have: platform_packages = \${common.platform_packages} ?"
        fail=1
    elif [ "$resolved" != "$wanted" ]; then
        echo "FAIL  $pkg pinned to $wanted but resolved $resolved"
        fail=1
    else
        echo "ok    $pkg $resolved (pinned)"
    fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "Build is NOT reproducible: CI and local can diverge silently."
    exit 1
fi
echo
echo "All pins enforced — CI and local builds should be byte-identical."

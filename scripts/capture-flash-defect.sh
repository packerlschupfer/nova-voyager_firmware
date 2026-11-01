#!/usr/bin/env bash
set -euo pipefail
#
# capture-flash-defect.sh — gather evidence BEFORE reflashing a wedged board.
#
# WHY THIS EXISTS
# ---------------
# A single flash cell at 0x08000488, inside the bootloader's first page, has
# gone bad six times on this machine: a 0->1 bit flip, repaired by a rewrite
# every time. The cause is unproven. The working hypothesis is a weak cell; the
# earlier "debugger misread" theory has been ruled out. It correlates with
# flash programming in general — incidents 3-6 followed plain st-flash app
# writes at 0x08003000, not DFU.
#
# Every incident so far was REFLASHED FIRST, which repaired the board and
# destroyed the evidence in the same stroke. Hence this script, and hence the
# rule: measure before reflashing. Run this, keep the output directory, and
# only then run `./flash_firmware.sh restore-bootloader`.
#
# The symptom it is for: the board boots once and then sits in the bootloader's
# Default_Handler with no serial output. Your firmware is not running, so no
# console command can help — this has to come over SWD.
#
# WHAT IT DELIBERATELY DOES NOT DO
# --------------------------------
# It never resets the target. `init; halt` only; no `reset halt`, no
# `reset init`, no `reset run`, and it leaves the core HALTED at the end so a
# second run or a manual look sees the same state. Only a power cycle clears
# the debug domain, so a reset here would throw away the thing being measured.
#
# Recipe and interpretation come from the nova-voyager_bootloader side, which
# owns the defect record. Reviewed there before this was considered done.
#
# Usage:
#   ./scripts/capture-flash-defect.sh [output-dir]
#
#   PI_HOST=""            probe on this machine
#   PI_HOST=user@host     probe on that host (default: the author's Pi)
#
cd "$(dirname "$0")/.." || { echo "cannot reach the repo root" >&2; exit 1; }

PI_HOST="${PI_HOST-pi@192.168.16.62}"

OUT="${1:-flash-defect-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

# Same convention as flash_firmware.sh: an explicitly empty PI_HOST means local.
remote_sh() {
    if [ -n "$PI_HOST" ]; then command ssh "$PI_HOST" "$1"; else bash -c "$1"; fi
}
remote_get() {
    if [ -n "$PI_HOST" ]; then command scp -q "$PI_HOST:$1" "$2"; else cp -f "$1" "$2"; fi
}

# Cortex-M debug registers. Addresses rather than names so the OpenOCD version
# in use cannot change what gets read.
DHCSR=0xE000EDF0   # bit 19 = S_LOCKUP
DBGMCU_CR=0xE0042004
CFSR=0xE000ED28    # write-1-to-clear
HFSR=0xE000ED2C    # bit 30 = FORCED, bit 1 = VECTTBL

BL_BASE=0x08000000
BL_SIZE=0x3000     # the whole 12 KB bootloader region, NOT a reference-image
                   # size — verify_bootloader_intact once compared only 792
                   # bytes and reported "pad clean" for a window that stopped
                   # short of the interesting address. This cannot inherit that.

echo "=== capture-flash-defect — output: $OUT ==="
echo
echo "Target: ${PI_HOST:-this machine}"
echo
echo "Commands that will run on the target (no reset is issued):"
cat <<'SHOWN'
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "reset_config none" \
        -c init -c halt \
        -c "reg pc" -c "reg xPSR" \
        -c "dump_image /tmp/fd_bootloader.bin 0x08000000 0x3000" \
        -c "mdw <DHCSR> 1" -c "mdw <DBGMCU_CR> 1" \
        -c "mdw <CFSR> 1"  -c "mdw <HFSR> 1" \
        -c "mww <CFSR> 0xFFFFFFFF" \
        -c resume -c "sleep 1000" -c halt \
        -c "reg pc" -c "mdw <CFSR> 1" -c "mdw <HFSR> 1" \
        -c shutdown
SHOWN
echo

# One session, so the halt state is continuous across every read.
#
# Order matters: pc and xPSR come first because they are free and xPSR's
# exception number says which handler the core is sitting in (3 = HardFault,
# which is what a FORCED bit in HFSR implies). The flash dump comes before the
# register writes so the evidence is on disk whatever happens afterwards.
OCD_LOG="/tmp/fd_openocd.log"
set +e
# `reset_config none` BEFORE init, deliberately. The stock
# target/stm32f1x.cfg sets `reset_config srst_nogate` (line 72 of OpenOCD
# 0.12.0's copy), which makes SRST available to anything that asks for a reset.
# Nothing here asks — but this script has to be safe on a bench where NRST IS
# wired, not just on the author's where it is not, and the one thing that would
# destroy the evidence is a reset. Overriding the config costs nothing and
# removes the possibility.
remote_sh "sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
    -c 'reset_config none' \
    -c 'init' -c 'halt' \
    -c 'echo \"--- pc/xPSR at halt ---\"' \
    -c 'reg pc' -c 'reg xPSR' \
    -c 'echo \"--- bootloader region dump ---\"' \
    -c 'dump_image /tmp/fd_bootloader.bin $BL_BASE $BL_SIZE' \
    -c 'echo \"--- DHCSR (bit19=S_LOCKUP) ---\"' -c 'mdw $DHCSR 1' \
    -c 'echo \"--- DBGMCU_CR (post-attach, debugger-set - see note) ---\"' -c 'mdw $DBGMCU_CR 1' \
    -c 'echo \"--- CFSR before clear ---\"' -c 'mdw $CFSR 1' \
    -c 'echo \"--- HFSR before clear ---\"' -c 'mdw $HFSR 1' \
    -c 'echo \"--- clearing CFSR, resuming 1s ---\"' \
    -c 'mww $CFSR 0xFFFFFFFF' \
    -c 'resume' -c 'sleep 1000' -c 'halt' \
    -c 'echo \"--- after resume: pc, CFSR, HFSR ---\"' \
    -c 'reg pc' -c 'mdw $CFSR 1' -c 'mdw $HFSR 1' \
    -c 'shutdown' > $OCD_LOG 2>&1"
rc=$?
set -e

remote_get "$OCD_LOG" "$OUT/openocd.log" 2>/dev/null || true
remote_get /tmp/fd_bootloader.bin "$OUT/bootloader-region.bin" 2>/dev/null || true

if [ ! -s "$OUT/openocd.log" ]; then
    echo "No OpenOCD output came back (exit $rc)." >&2
    echo "If the probe is not attached or the board is unpowered, that is the" >&2
    echo "first thing to check — and note an unpowered board looks identical" >&2
    echo "to a wedged one from here." >&2
    exit 1
fi

cat "$OUT/openocd.log"
echo

if [ -s "$OUT/bootloader-region.bin" ]; then
    sz=$(stat -c%s "$OUT/bootloader-region.bin")
    echo "Bootloader region captured: $sz bytes -> $OUT/bootloader-region.bin"
    # The byte that has rotted six times, and its neighbours.
    echo "Bytes around 0x08000488:"
    od -Ax -tx1 -j $((0x480)) -N 32 "$OUT/bootloader-region.bin" | sed 's/^/    /'
else
    echo "WARNING: no flash dump came back - the most important artefact is missing." >&2
fi

cat <<'NOTES'

NOTES ON READING THIS
  - DBGMCU_CR read here is CONTAMINATED. OpenOCD's stm32f1x config sets
    DBG_IWDG_STOP and friends during attach, so a 0x307 read through the
    debugger says nothing about the state before attach. The meaningful value
    is the one the firmware prints at boot, without a debugger involved.
  - xPSR's low bits are the exception number: 3 = HardFault. A FORCED bit in
    HFSR with pc in Default_Handler is the expected signature of this defect.
  - The core is left HALTED. Nothing here reset it, and only a power cycle
    clears the debug domain, so a second run sees the same state.
  - Compare bootloader-region.bin against a known-good build before concluding
    anything about which bytes moved:
        cmp -l <known-good>.bin flash-defect-*/bootloader-region.bin | head
    Sizes: nova_bootloader_120 792 B, nova_bootloader 3540 B,
    nova_bootloader_pad 4420 B. A different SIZE means a different build, not
    corruption.

ONLY NOW is it reasonable to run:
    ./flash_firmware.sh restore-bootloader
NOTES

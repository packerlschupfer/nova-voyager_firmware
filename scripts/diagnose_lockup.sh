#!/bin/bash
# Capture everything needed to diagnose a board lockup — BEFORE reflashing.
#
# Reflashing cures the symptom and destroys the evidence. Two lockups on
# 2026-08-29 were each reflashed away before the discriminating measurement was
# taken, which is why the cause is still unknown. Run this first, every time.
#
# Usage:  scripts/diagnose_lockup.sh [outdir]
set -u
PI_HOST="${PI_HOST:-pi@192.168.16.62}"
OUT="${1:-./lockup_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT"
OCD="sudo openocd -f interface/stlink.cfg -c 'transport select swd' -f target/stm32f1x.cfg"

echo "=== capturing to $OUT ==="

# 1. Debug/fault state FIRST, before anything perturbs it.
#    FPB comparators are the leading suspect and a reflash clears them.
ssh "$PI_HOST" "$OCD -c 'init' -c 'halt' \
  -c 'echo {== regs ==}'   -c 'reg pc' -c 'reg sp' -c 'reg lr' \
  -c 'echo {== FPB: FP_CTRL FP_REMAP FP_COMP0..7 ==}' -c 'mdw 0xE0002000 10' \
  -c 'echo {== DHCSR ==}'  -c 'mdw 0xE000EDF0 1' \
  -c 'echo {== DEMCR ==}'  -c 'mdw 0xE000EDFC 1' \
  -c 'echo {== ICSR ==}'   -c 'mdw 0xE000ED04 1' \
  -c 'echo {== HFSR ==}'   -c 'mdw 0xE000ED2C 1' \
  -c 'echo {== CFSR ==}'   -c 'mdw 0xE000ED28 1' \
  -c 'echo {== BFAR (only valid if CFSR bit15 BFARVALID) ==}' -c 'mdw 0xE000ED38 1' \
  -c 'echo {== MMFAR (only valid if CFSR bit7 MMARVALID) ==}' -c 'mdw 0xE000ED34 1' \
  -c 'echo {== ACTLR ==}'  -c 'mdw 0xE000E008 1' \
  -c 'echo {== DBGMCU_CR: bit8 DBG_IWDG_STOP freezes the IWDG and SURVIVES reset ==}' \
  -c 'mdw 0xE0042004 1' \
  -c 'echo {== RCC_CR / RCC_CFGR ==}' -c 'mdw 0x40021000 2' \
  -c 'resume' -c 'shutdown'" 2>&1 | tee "$OUT/registers.txt"

# 2. Flash images read with the core HALTED. st-flash's own verify is NOT
#    evidence — it reported success on writes later found damaged.
ssh "$PI_HOST" "$OCD -c 'init' -c 'halt' \
  -c 'dump_image /tmp/lk_bl.bin 0x08000000 4096' \
  -c 'dump_image /tmp/lk_fw.bin 0x08003000 131072' -c 'resume' -c 'shutdown'" 2>&1 | tail -3
ssh "$PI_HOST" "sudo chmod 644 /tmp/lk_bl.bin /tmp/lk_fw.bin"
scp -q "$PI_HOST:/tmp/lk_bl.bin" "$OUT/" && scp -q "$PI_HOST:/tmp/lk_fw.bin" "$OUT/"

# 3. FPB read-path check.
#
#    *** KNOWN LIMITATION, measured 2026-08-29: this CANNOT detect a stale
#    comparator. OpenOCD clears FP_COMP on attach AND on reset. Verified:
#      armed FP_COMP0      = 0x4803F001
#      detach/re-attach    = 0x00000000
#      reset halt (same session) = 0x00000000
#    So by the time this script can read them, any comparators that existed
#    are already gone, and both dumps below will agree whatever the truth was.
#    An "IDENTICAL" verdict here is therefore NOT evidence that the flash was
#    genuinely damaged - it is the only result this test can produce.
#
#    Kept because the dumps themselves are still worth having, and because a
#    DIFFERENT result would be a real (if unexpected) finding. Do not read the
#    identical case as confirmation of anything.
#
#    The same limitation applies to the FP_COMP values captured in step 1.
#    They are read after attach, so they are always zero. There is no
#    debugger-based way to observe inherited FPB state on this rig; the only
#    route is the firmware reading and reporting it itself, the way it now
#    does for DBGMCU_CR.
echo "=== FPB-disabled re-read (the discriminating test) ===" | tee "$OUT/fpb_test.txt"
ssh "$PI_HOST" "$OCD -c 'init' -c 'halt' \
  -c 'echo {-- before: FP_CTRL --}' -c 'mdw 0xE0002000 1' \
  -c 'echo {-- bootloader first 64 words, FPB ON --}'  -c 'mdw 0x08000400 64' \
  -c 'echo {-- disabling FPB (KEY=1 ENABLE=0) --}' -c 'mww 0xE0002000 0x00000002' -c 'mdw 0xE0002000 1' \
  -c 'echo {-- same words, FPB OFF --}' -c 'mdw 0x08000400 64' \
  -c 'echo {-- restoring FP_CTRL --}' -c 'mww 0xE0002000 0x00000003' -c 'mdw 0xE0002000 1' \
  -c 'resume' -c 'shutdown'" 2>&1 | tee -a "$OUT/fpb_test.txt"

# 4. Compare the two dumps automatically. Leaving this to the operator is how
#    it gets skipped at 2am.
python3 - "$OUT/fpb_test.txt" <<'PYEOF'
import re, sys
t = open(sys.argv[1], errors='ignore').read()
# the two word dumps, in order: FPB ON then FPB OFF
parts = re.split(r'--\s*same words, FPB OFF\s*--', t)
def words(chunk):
    return re.findall(r'^0x0800[0-9a-f]+:((?:\s+[0-9a-f]{8})+)', chunk, re.M)
def flat(ws):
    return [w for line in ws for w in line.split()]
if len(parts) < 2:
    print("VERDICT: could not parse both dumps - inspect fpb_test.txt by hand")
    sys.exit(0)
on, off = flat(words(parts[0])), flat(words(parts[1]))
print()
print("words captured: FPB ON=%d  FPB OFF=%d" % (len(on), len(off)))
if not on or len(on) != len(off):
    print("VERDICT: dumps not comparable - inspect by hand")
elif on == off:
    print("VERDICT: IDENTICAL -> expected, and NOT informative.")
    print("  OpenOCD clears FP_COMP on attach and on reset, so no comparator")
    print("  can survive to affect this read. This result does not distinguish")
    print("  genuine flash damage from a redirect. See the header comment.")
else:
    diff = [i for i,(a,b) in enumerate(zip(on,off)) if a != b]
    print("VERDICT: DIFFERENT at %d word(s) -> reads DO pass through FPB." % len(diff))
    print("  The flash was never damaged; this is a tooling artifact.")
    for i in diff[:8]:
        print("   word %2d: FPB-ON=%s  FPB-OFF=%s" % (i, on[i], off[i]))
PYEOF

echo
echo "Captured to $OUT. Only after reading the above should you reflash."

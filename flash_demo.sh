#!/bin/bash
# Build, flash, and run the READ-ONLY DEMO firmware over a Raspberry Pi.
#
# The demo build (env: nova_voyager_demo) writes NOTHING to the EEPROM/flash.
# This script flashes it via the Pi-attached ST-Link and captures a backup of
# the EEPROM by issuing the EEDUMP command over serial (on demand, not on boot).
#
# Usage:
#   ./flash_demo.sh                 # build + flash + capture EEPROM backup
#   ./flash_demo.sh capture         # just capture the EEPROM backup (EEDUMP)
#   ./flash_demo.sh console         # open interactive serial console (picocom)
#   PI_HOST=pi@1.2.3.4 ./flash_demo.sh   # override Pi host
set -e

PI_HOST="${PI_HOST:-pi@192.168.16.62}"
SERIAL="${SERIAL:-/dev/ttyNova}"
ENV="nova_voyager_demo"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/.pio/build/$ENV/firmware.bin"
MODE="${1:-flash}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
step() { echo -e "${GREEN}==> $1${NC}"; }
warn() { echo -e "${YELLOW}$1${NC}"; }

# Capture an EEPROM backup to a local timestamped file by issuing EEDUMP over
# serial (resets the board first so the freshly-flashed firmware is running).
capture_backup() {
    local out="$SCRIPT_DIR/eeprom_backup_$(date +%Y%m%d_%H%M%S).txt"
    step "Capturing EEPROM backup (EEDUMP) -> $(basename "$out")"
    ssh "$PI_HOST" "python3 - '$SERIAL'" <<'PY' > "$out" 2>/dev/null || true
import serial, subprocess, time, sys
port = sys.argv[1]
s = serial.Serial(port, 9600, timeout=2)
subprocess.run(['sudo', 'st-flash', 'reset'], capture_output=True)
time.sleep(3)                 # let the firmware boot
s.reset_input_buffer()
s.write(b'EEDUMP\r\n')
deadline = time.time() + 5
buf = b''
while time.time() < deadline:
    buf += s.read(512)
s.close()
sys.stdout.write(buf.decode(errors='replace'))
PY
    if grep -q "EEPROM DUMP" "$out" 2>/dev/null; then
        step "Backup captured ($(wc -l < "$out") lines). Saved: $out"
    else
        warn "No EEDUMP output seen — is the demo build running and serial free? Saved raw to $out"
    fi
}

case "$MODE" in
    flash)
        step "Building $ENV..."
        cd "$SCRIPT_DIR" && pio run -e "$ENV"
        step "Flashing to $PI_HOST..."
        scp "$BIN" "$PI_HOST:/tmp/firmware.bin"
        ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000" 2>&1 | tail -1
        capture_backup
        echo
        step "Demo running. Try over serial: STATUS / DIAG / MREAD / REGSCAN / SNIFF / EEDUMP"
        step "Showcase: menu > Showcas, or  GAME D"
        ;;
    capture)
        capture_backup
        ;;
    console)
        step "Opening serial console (Ctrl-A Ctrl-X to exit)..."
        ssh -t "$PI_HOST" "picocom $SERIAL -b 9600 -q"
        ;;
    *)
        echo "Usage: $0 [flash|capture|console]"
        exit 1
        ;;
esac

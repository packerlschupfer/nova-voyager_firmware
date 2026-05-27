#!/bin/bash
# Nova Voyager Firmware Switching Script
# Usage: ./flash_firmware.sh [original|custom|erase|unlock]
#
# GD32F303 Flash Protection Notes:
# ================================
# The GD32F303 has persistent flash write protection that survives mass erase.
# Even after st-flash erase, you may get "Flash memory is write protected" errors.
#
# Solution: Unlock the flash controller via direct register writes using OpenOCD.
# This script includes an 'unlock' command and automatically unlocks before flashing.
#
# Flash Controller Registers (GD32F303 / STM32F1xx compatible):
#   FLASH_KEYR    = 0x40022004  (Flash Key Register)
#   FLASH_OPTKEYR = 0x40022008  (Option Byte Key Register)
#   Unlock keys: 0x45670123 followed by 0xCDEF89AB

set -e

PI_HOST="pi@192.168.16.62"

# Auto-detect project directory: use script location if it has platformio.ini,
# otherwise fall back to the old dev location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/platformio.ini" ]; then
    PROJECT_DIR="$SCRIPT_DIR"
    BOOTLOADER_DIR="${PROJECT_DIR}/../nova-voyager_bootloader"
    FIRMWARE_DIR="$PROJECT_DIR"
else
    PROJECT_DIR="/home/mrnice/Documents/Projects/teknatool_voyager"
    BOOTLOADER_DIR="$PROJECT_DIR/nova_bootloader"
    FIRMWARE_DIR="$PROJECT_DIR/nova_firmware"
fi
# Stock Teknatool images and the OEM bootloader backup live in the
# teknatool_voyager tree, NOT in whichever project directory this script
# happens to sit in. Resolving them from PROJECT_DIR meant "flash_firmware.sh
# original" died with "Firmware file not found" whenever the script was run
# from the git repo — which is the normal case. Look where the assets actually
# are, and fall back to the project dir for anyone who keeps them alongside.
ASSETS_DIR="/home/mrnice/Documents/Projects/teknatool_voyager"
if [ ! -d "$ASSETS_DIR/firmware/official" ] && [ -d "$PROJECT_DIR/firmware/official" ]; then
    ASSETS_DIR="$PROJECT_DIR"
fi
ORIGINAL_BOOTLOADER="$ASSETS_DIR/firmware/bootloader_gd32_backup.bin"
# Note: CUSTOM_FW is set after FIRMWARE_ENV is determined

# ---------------------------------------------------------------------------
# verify_bootloader_intact — check the BOOTLOADER region after flashing.
#
# WHY: five times now (three on 2026-08-30 alone), a routine app flash has been
# followed by the board sitting in the bootloader's Default_Handler with
# pc=0x08000488, VTOR=0, and one byte at 0x08000488 reading 0x08 where the image
# holds 0x00. The app write starts at 0x08003000 and never touches that address,
# and a 0->1 bit flip is not something flash PROGRAMMING can do at all — so the
# mechanism is still unexplained. What IS certain is the cost: each occurrence
# looks exactly like a dead board or a firmware regression, and has taken
# several minutes of halted-core forensics to tell apart.
#
# Checking costs about a second. Do it every flash, so a corrupted bootloader is
# reported as itself rather than diagnosed from symptoms later.
# ---------------------------------------------------------------------------
# Sacrificial canary pad in the diagnostic bootloader (env nova_bootloader_pad):
# file offsets 0x130..0x49F, 880 bytes, filled 0xA5. Default_Handler was moved
# from 0x08000488 to 0x080007F8, so the historically-damaged addresses now hold
# pad instead of live code. Classification is purely POSITIONAL — a difference
# inside the pad is a canary hit and the board still runs; anywhere else is live
# code and the board is dead. Set PAD_LO/PAD_HI to 0 when running the
# production bootloader, which has no pad.
PAD_LO=$((0x130))
PAD_HI=$((0x49F))

verify_bootloader_intact() {
    local img="${BOOTLOADER_BIN:-/home/mrnice/git/github/nova-voyager_bootloader/.pio/build/nova_bootloader_pad/firmware.bin}"
    if [ ! -f "$img" ]; then
        echo "  (bootloader image not found - skipping integrity check)"
        return 0
    fi
    local size
    size=$(stat -c%s "$img")

    echo_step "Verifying bootloader region intact (${size} bytes)..."
    scp -q "$img" "$PI_HOST:/tmp/bl_reference.bin"
    if ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
            -c 'init' -c 'halt' \
            -c 'dump_image /tmp/bl_readback.bin 0x08000000 ${size}' \
            -c 'reset run' -c 'shutdown' >/dev/null 2>&1
            cmp -s /tmp/bl_readback.bin /tmp/bl_reference.bin"; then
        echo "  Bootloader intact (pad clean)."
        return 0
    fi

    # cmp -l gives 1-based byte offsets with OCTAL values.
    local diffs
    diffs=$(ssh "$PI_HOST" "cmp -l /tmp/bl_reference.bin /tmp/bl_readback.bin 2>/dev/null | head -60")

    local outside
    outside=$(echo "$diffs" | awk -v lo="$PAD_LO" -v hi="$PAD_HI" \
        '{ off = $1 - 1; if (off < lo || off > hi) c++ } END { print c+0 }')

    echo ""
    if [ "$outside" -eq 0 ] && [ -n "$diffs" ]; then
        echo "  === CANARY HIT — pad only, THE BOARD IS FINE ==="
        echo "  The weak cell rotted inside the sacrificial pad, where nothing"
        echo "  executes. This is the diagnostic working: it means the cell is"
        echo "  still degrading, and that the fixed-cell theory is holding."
    else
        echo "  *** LIVE CODE CORRUPTED — outside the pad ***"
        echo "  $outside byte(s) fall outside the pad. The board will boot once"
        echo "  and then sit in Default_Handler (now 0x080007F8) with no serial."
        echo "  If the damage is AT 0x080007F8 the corruption tracks the symbol"
        echo "  and the fixed-cell theory is dead — record that carefully."
    fi
    echo ""
    echo "  Differences (flash address: expected -> actual):"
    echo "$diffs" | awk -v lo="$PAD_LO" -v hi="$PAD_HI" \
      '{ off = $1 - 1;
         printf "    0x%08X:  0x%02X -> 0x%02X   %s\n",
                0x08000000 + off, strtonum("0" $2), strtonum("0" $3),
                (off >= lo && off <= hi) ? "[pad]" : "[LIVE CODE]" }'
    echo ""
    echo "  Recover with:  ./flash_firmware.sh restore-bootloader"
    echo ""
    return 1
}

# Rewrite the bootloader from its reference image and verify halted.
restore_bootloader() {
    local img="${BOOTLOADER_BIN:-/home/mrnice/git/github/nova-voyager_bootloader/.pio/build/nova_bootloader_pad/firmware.bin}"
    local size
    size=$(stat -c%s "$img")
    echo_step "Restoring bootloader (${size} bytes)..."
    scp -q "$img" "$PI_HOST:/tmp/bl_reference.bin"
    ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c 'init' -c 'halt' \
        -c 'mww 0x40022004 0x45670123' -c 'mww 0x40022004 0xCDEF89AB' \
        -c 'flash write_image erase /tmp/bl_reference.bin 0x08000000' \
        -c 'dump_image /tmp/bl_readback.bin 0x08000000 ${size}' \
        -c 'reset run' -c 'shutdown' 2>&1 | grep -E 'wrote|Error'
        cmp -s /tmp/bl_readback.bin /tmp/bl_reference.bin && echo '  Halted verify: IDENTICAL'"
}

# Bootloader options:
#   nova_bootloader     - 72MHz, USB DFU works
#   nova_bootloader_120 - 120MHz, ST-Link only (default)
BOOTLOADER_ENV="nova_bootloader_120"
FIRMWARE_ENV="release_120"  # Must match bootloader clock speed!

# Parse --dfu flag to use 72MHz bootloader with USB DFU support
for arg in "$@"; do
    if [[ "$arg" == "--dfu" || "$arg" == "--72mhz" ]]; then
        BOOTLOADER_ENV="nova_bootloader"
        FIRMWARE_ENV="nova_voyager"  # 72MHz firmware
        echo "Using 72MHz bootloader (USB DFU enabled)"
    fi
done

# Parse --debug to flash the debug build instead of release. Still a 120MHz
# env, so it satisfies the "must match bootloader clock speed" rule above.
# Needed because CMD_FLAG_DEBUG commands (TAPTEST/TAPSTOP/TAPSIM, JOG, MQ...)
# are refused at dispatch on a release build, and LOG_LEVEL=1 suppresses the
# debug logging you usually want when testing on the machine.
for arg in "$@"; do
    if [[ "$arg" == "--debug" ]]; then
        FIRMWARE_ENV="debug_120"
        echo "Using DEBUG firmware build (debug commands + full logging)"
    fi
done

# Remove flags from args
set -- "${@/--dfu/}"
set -- "${@/--72mhz/}"
set -- "${@/--debug/}"

CUSTOM_BOOTLOADER="$BOOTLOADER_DIR/.pio/build/$BOOTLOADER_ENV/firmware.bin"
CUSTOM_FW="$FIRMWARE_DIR/.pio/build/$FIRMWARE_ENV/firmware.bin"

# Official firmware versions (all CG = Chuck Guard variants)
# Release notes:
#   R2P05x - Original release (May 2018)
#   R2P06E - Increased jam timeout before motor self-stop
#   R2P06K - Latest (current production)
declare -A FW_VERSIONS=(
    ["r2p05x"]="$ASSETS_DIR/firmware/official/firmware_r2p05x_cg.bin"
    ["r2p06e"]="$ASSETS_DIR/firmware/official/firmware_r2p06e_cg.bin"
    ["r2p06k"]="$ASSETS_DIR/firmware/official/firmware_r2p06k_cg.bin"
)
DEFAULT_FW_VERSION="r2p06k"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo_step() { echo -e "${GREEN}==> $1${NC}"; }
echo_warn() { echo -e "${YELLOW}WARNING: $1${NC}"; }
echo_err() { echo -e "${RED}ERROR: $1${NC}"; }
echo_info() { echo -e "${CYAN}$1${NC}"; }

# Unlock GD32 flash controller via register writes
# This is required when flash protection persists after mass erase
unlock_flash() {
    echo_step "Unlocking GD32 flash controller..."
    ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg \
        -c 'transport select swd' \
        -f target/stm32f1x.cfg \
        -c 'init; reset halt; \
            mww 0x40022004 0x45670123; mww 0x40022004 0xCDEF89AB; \
            mww 0x40022008 0x45670123; mww 0x40022008 0xCDEF89AB; \
            shutdown'" 2>&1 | grep -E "(Info|Error)" | head -5 || true
    echo_step "Flash controller unlocked"
}

mass_erase() {
    echo_step "Mass erasing flash..."
    echo_warn "MASS ERASE wipes ENTIRE flash (bootloader + firmware)!"
    echo_warn "You must reflash BOTH after this operation."
    ssh "$PI_HOST" "sudo st-flash --connect-under-reset erase" || true
}

# Flash immediately after erase - NO reset in between!
# Use OpenOCD to unlock, erase, and write in a single session
erase_and_flash_both() {
    local bl_file="$1"
    local fw_file="$2"

    # OpenOCD to unlock and erase; st-flash to write. Both halves of that split
    # are load-bearing and were learned the hard way on 2026-09-05:
    #
    #  * OpenOCD's own `program` fails on this GD32. Its flash probe reports
    #    "STM32 flash size failed, probe inaccurate - assuming 512k", then the
    #    write times out. The previous version of this function used `program`
    #    for both images: it erased the chip, failed to write, and left the
    #    board completely blank with no message saying so. st-flash sizes the
    #    part correctly (256k) and has written every image on this machine all
    #    session.
    #
    #  * The erase must still be OpenOCD, because it needs `stm32f1x unlock 0`
    #    first. The STOCK firmware re-enables flash write protection at
    #    runtime, so st-flash alone answers "Flash memory is write protected"
    #    whenever stock is running. Erasing under OpenOCD leaves nothing
    #    running to re-protect, which is why the writes then succeed.
    scp "$bl_file" "$PI_HOST:/tmp/bootloader.bin"
    scp "$fw_file" "$PI_HOST:/tmp/firmware.bin"

    echo_step "Unlocking and erasing (OpenOCD)..."
    local erase_out
    erase_out=$(ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg \
        -c 'transport select swd' \
        -c 'adapter speed 1800' \
        -f target/stm32f1x.cfg \
        -c 'init' \
        -c 'reset halt' \
        -c 'stm32f1x unlock 0' \
        -c 'reset halt' \
        -c 'flash erase_sector 0 0 last' \
        -c 'shutdown'" 2>&1)
    echo "$erase_out" | grep -E "unlocked|erased sectors|Error" | head -5

    if ! echo "$erase_out" | grep -q "erased sectors"; then
        echo_err "ERASE FAILED — the board should be untouched."
        echo_err "Power-cycle the drill press and try again."
        exit 1
    fi

    # From here the chip is BLANK. Any failure below leaves it that way, so say
    # so explicitly rather than exiting with a bare error — a silent blank board
    # looks identical to a dead one.
    echo_step "Writing bootloader + firmware (st-flash)..."
    local write_out
    write_out=$(ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/bootloader.bin 0x08000000 2>&1; \
                                echo '---SPLIT---'; \
                                sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000 2>&1" 2>&1)
    local wrote
    wrote=$(echo "$write_out" | grep -c "jolly good")
    echo "$write_out" | grep -Ei "jolly good|error" | head -4

    if [ "$wrote" -ne 2 ]; then
        echo_err "WRITE FAILED after a successful erase — THE BOARD IS NOW BLANK."
        echo_err "It is not damaged. Recover with:"
        echo_err "  ssh $PI_HOST \"sudo st-flash --flash=256k write /tmp/bootloader.bin 0x08000000\""
        echo_err "  ssh $PI_HOST \"sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000\""
        echo_err "  ssh $PI_HOST \"sudo st-flash reset\""
        exit 1
    fi

    ssh "$PI_HOST" "sudo st-flash reset" >/dev/null 2>&1

    # Read back both vector tables. "Verified" from the writer is not evidence
    # the part actually boots — see docs on the recurring bootloader corruption.
    echo_step "Verifying vector tables..."
    ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg -c 'transport select swd' \
        -f target/stm32f1x.cfg -c 'init' -c 'mdw 0x08000000 2' -c 'mdw 0x08003000 2' \
        -c 'shutdown'" 2>&1 | grep -E "^0x0800"
}

flash_bootloader() {
    local bl_file="$1"
    local bl_name="$2"
    echo_step "Flashing $bl_name bootloader..."
    scp "$bl_file" "$PI_HOST:/tmp/bootloader.bin"
    ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/bootloader.bin 0x08000000"
}

flash_firmware() {
    local fw_file="$1"
    local fw_name="$2"
    echo_step "Flashing $fw_name firmware..."
    scp "$fw_file" "$PI_HOST:/tmp/firmware.bin"
    ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000"
}

list_versions() {
    echo "Available official firmware versions:"
    echo ""
    echo "  r2p05x  - Original release (May 2018)"
    echo "  r2p06e  - Increased jam timeout before motor self-stop"
    echo "  r2p06k  - Latest production firmware (default)"
    echo ""
    echo "All versions are CG (Chuck Guard) variants."
}

case "$1" in
    original)
        # Get version from $2 or use default
        VERSION="${2:-$DEFAULT_FW_VERSION}"
        VERSION="${VERSION,,}"  # lowercase

        # Check if version exists
        if [[ -z "${FW_VERSIONS[$VERSION]}" ]]; then
            echo_err "Unknown firmware version: $VERSION"
            echo ""
            list_versions
            exit 1
        fi

        ORIGINAL_FW="${FW_VERSIONS[$VERSION]}"

        # Check if file exists
        if [[ ! -f "$ORIGINAL_FW" ]]; then
            echo_err "Firmware file not found: $ORIGINAL_FW"
            exit 1
        fi

        echo_step "Switching to ORIGINAL Teknatool firmware ($VERSION)"
        echo ""
        echo_info "Firmware: $ORIGINAL_FW"
        echo_info "Size: $(stat -c%s "$ORIGINAL_FW") bytes"
        echo ""
        echo_warn "This will install the original Teknatool bootloader + firmware"
        # Use atomic erase+flash to avoid protection issues (same as custom)
        erase_and_flash_both "$ORIGINAL_BOOTLOADER" "$ORIGINAL_FW"
        echo_step "Done! Original firmware ($VERSION) flashed and reset automatically."
        ;;

    custom)
        echo_step "Switching to CUSTOM firmware"
        echo ""

        # Build first
        echo_step "Building custom bootloader ($BOOTLOADER_ENV)..."
        cd "$BOOTLOADER_DIR" && pio run -e "$BOOTLOADER_ENV"

        echo_step "Building custom firmware ($FIRMWARE_ENV)..."
        cd "$FIRMWARE_DIR" && pio run -e "$FIRMWARE_ENV"

        # Erase and flash immediately - no power cycle needed!
        erase_and_flash_both "$CUSTOM_BOOTLOADER" "$CUSTOM_FW"
        echo_step "Done! Custom firmware flashed and reset automatically."
        ;;

    erase)
        echo_step "Mass erasing flash only"
        mass_erase
        echo_step "Done! Flash erased."
        echo_step "After power cycle, run './flash_firmware.sh unlock' before flashing."
        ;;

    unlock)
        echo_step "Unlocking flash controller only"
        echo ""
        echo "Use this if you get 'Flash memory is write protected' errors."
        echo "The unlock is temporary and resets on power cycle."
        echo ""
        unlock_flash
        echo ""
        echo "You can now flash with st-flash:"
        echo "  st-flash --flash=256k write bootloader.bin 0x08000000"
        echo "  st-flash --flash=256k write firmware.bin 0x08003000"
        ;;

    quick)
        # Quick flash - assumes device UNLOCKED (custom firmware already running)
        # Uses fast st-flash, NO OpenOCD unlock needed
        echo_step "Quick flash custom firmware ($FIRMWARE_ENV, assumes unlocked)"
        cd "$FIRMWARE_DIR" && pio run -e "$FIRMWARE_ENV"
        # ---------------------------------------------------------------
        # st-flash, deliberately.
        #
        # TRIED AND REVERTED 2026-08-31: all five bootloader-corruption events
        # have followed an st-flash app write, while the OpenOCD writes used to
        # REPAIR them have never corrupted anything — so switching writers
        # looked like a cheap test of the "GD32 programmed with STM32F1 timing"
        # hypothesis. OpenOCD cannot do this write: it aborts with
        #
        #   flash write algorithm aborted by target
        #   clearing lockup after double fault
        #   flash write failed just before address 0x20004ff0
        #
        # Its loader runs from an SRAM working area and faults partway through
        # the ~100 KB image, while handling the 3.5 KB bootloader fine.
        #
        # RETESTED 2026-08-31, and OpenOCD simply cannot do this write:
        #   - default 4 KB work area  -> "algorithm aborted by target", double fault
        #   - WORKAREASIZE 0x4000     -> "algorithm aborted", "timed out waiting
        #                                 for target halted"
        #   - work-area-size 0 (host-driven halfword programming, no algorithm)
        #                             -> hung; had to be killed
        # Each attempt left the app region unwritten and the board silent, and
        # st-flash rewrote it every time. So the writer cannot be swapped, and
        # the "GD32 programmed with STM32F1 timing" hypothesis stays untested by
        # this route. If it is ever worth chasing again, the remaining ideas are
        # a GD32-aware programmer (gd32-dfu / stm32flash over the bootloader) or
        # a newer st-flash with GD32 support — not more OpenOCD tuning.
        #
        # Until then st-flash stays, and the integrity check below is what makes
        # the corruption cheap to spot rather than cheap to cause.
        # ---------------------------------------------------------------
        # EXPERIMENT 2026-08-31: halt the core AT RESET before programming.
        #
        # st-flash HALTS the target but does not reset it, so its SRAM flash
        # loader executes with whatever clock the running application left
        # configured — for us that is HCLK 120 MHz with FLASH_ACR = 3 wait
        # states (init.c). st-flash programs this part as "F1xx_HD", i.e. with
        # STM32F1 assumptions, but it is a GD32F303 whose flash timing is not
        # identical. Programming at 120 MHz under STM32F1 timing is a plausible
        # source of marginally-programmed cells, which is what the recurring
        # 0x08000488 corruption looks like (a bit that never took properly and
        # later reads back as 1).
        #
        # `reset halt` leaves the core stopped at the reset vector, before
        # clock_init() runs — so the loader programs at the 8 MHz HSI default
        # with zero wait states, the most conservative condition available.
        # Costs about a second. If the corruption stops recurring, that is the
        # evidence; if it does not, this is harmless and can come out.
        echo_step "Reset-halt (program at HSI 8 MHz, not 120 MHz)..."
        ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
            -c 'init' -c 'reset halt' -c 'shutdown' >/dev/null 2>&1" || true

        echo_step "Fast write with st-flash..."
        scp -q "$CUSTOM_FW" "$PI_HOST:/tmp/firmware.bin"
        ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000"
        echo_step "Resetting MCU..."
        ssh "$PI_HOST" "sudo st-flash reset"
        verify_bootloader_intact
        echo_step "Done!"
        ;;

    restore-bootloader)
        restore_bootloader
        ;;

    quickboot)
        # Flash both bootloader and firmware FAST (assumes unlocked)
        echo_step "Quick flash custom bootloader ($BOOTLOADER_ENV) + firmware ($FIRMWARE_ENV, assumes unlocked)"
        cd "$BOOTLOADER_DIR" && pio run -e "$BOOTLOADER_ENV"
        cd "$FIRMWARE_DIR" && pio run -e "$FIRMWARE_ENV"
        echo_step "Fast write with st-flash..."
        scp "$CUSTOM_BOOTLOADER" "$PI_HOST:/tmp/bootloader.bin"
        scp "$CUSTOM_FW" "$PI_HOST:/tmp/firmware.bin"
        ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/bootloader.bin 0x08000000 && \
            sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000"
        echo_step "Resetting MCU..."
        ssh "$PI_HOST" "sudo st-flash reset"
        verify_bootloader_intact
        echo_step "Done!"
        ;;

    versions|list)
        list_versions
        ;;

    *)
        echo "Nova Voyager Firmware Switching Script"
        echo ""
        echo "Usage: $0 <command> [options] [version]"
        echo ""
        echo "Commands:"
        echo "  original [ver] - Flash original Teknatool firmware"
        echo "                   Versions: r2p05x, r2p06e, r2p06k (default)"
        echo "  custom         - Flash custom firmware (builds first)"
        echo "  quick          - Flash custom FAST (~2s, assumes unlocked)"
        echo "  restore-bootloader - Rewrite the bootloader after the 0x08000488 corruption"
        echo "  quickboot      - Flash bootloader + firmware FAST"
        echo "  erase          - Mass erase flash (DANGER!)"
        echo "  unlock         - Unlock flash controller"
        echo "  versions       - List available official versions"
        echo ""
        echo "Options:"
        echo "  --dfu, --72mhz - Use 72MHz bootloader with USB DFU support"
        echo "                   Default: 120MHz bootloader (ST-Link only)"
        echo ""
        echo "Examples:"
        echo "  $0 original           # Flash latest (r2p06k)"
        echo "  $0 original r2p05x    # Flash oldest version"
        echo "  $0 custom             # Flash custom (120MHz bootloader)"
        echo "  $0 custom --dfu       # Flash custom (72MHz, USB DFU)"
        echo ""
        echo "Official Firmware Versions:"
        echo "  r2p05x  - Original (May 2018)"
        echo "  r2p06e  - Jam timeout fix"
        echo "  r2p06k  - Latest production"
        echo ""
        echo "Bootloader Options:"
        echo "  Default (120MHz) - Faster CPU, ST-Link flashing only"
        echo "  --dfu   (72MHz)  - USB DFU support for firmware updates"
        echo ""
        echo "SPEED GUIDE:"
        echo "  First time:    Use 'custom' (slow, unlocks protection)"
        echo "  Development:   Use 'quick' (100× faster)"
        echo "  Switch to OEM: Use 'original [version]'"
        exit 1
        ;;
esac

# Recovery flash after mass erase (flashes both bootloader and firmware)
recover() {
    echo_color "$GREEN" "==> RECOVERY FLASH (Bootloader + Firmware)"
    echo ""
    echo "This will:"
    echo "  1. Flash 120MHz bootloader at 0x08000000"
    echo "  2. Flash custom firmware at 0x08003000"
    echo ""

    # Copy files to Pi
    echo_color "$YELLOW" "Copying bootloader and firmware to Pi..."
    scp nova_bootloader/.pio/build/nova_bootloader_120/firmware.bin pi@$PI_IP:/tmp/bootloader_120.bin
    scp .pio/build/release_120/firmware.bin pi@$PI_IP:/tmp/firmware.bin

    # Flash bootloader
    echo_color "$GREEN" "==> Flashing 120MHz bootloader..."
    ssh pi@$PI_IP "st-flash write /tmp/bootloader_120.bin 0x08000000"

    if [ $? -ne 0 ]; then
        echo_color "$RED" "ERROR: Bootloader flash failed!"
        echo "Try: ./flash_firmware.sh unlock"
        exit 1
    fi

    # Flash firmware
    echo_color "$GREEN" "==> Flashing custom firmware..."
    ssh pi@$PI_IP "st-flash write /tmp/firmware.bin 0x08003000"

    if [ $? -ne 0 ]; then
        echo_color "$RED" "ERROR: Firmware flash failed!"
        exit 1
    fi

    # Reset
    echo_color "$GREEN" "==> Resetting MCU..."
    ssh pi@$PI_IP "st-flash reset"

    echo_color "$GREEN" "==> Recovery complete!"
    echo ""
    echo "System should boot normally. Check serial console."
}

# Add recover to case statement
if [ "$1" == "recover" ]; then
    recover
    exit 0
fi

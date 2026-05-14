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
PROJECT_DIR="/home/mrnice/Documents/Projects/teknatool_voyager"
ORIGINAL_BOOTLOADER="$PROJECT_DIR/firmware/bootloader_gd32_backup.bin"
# Note: CUSTOM_FW is set after FIRMWARE_ENV is determined

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
# Remove flags from args
set -- "${@/--dfu/}"
set -- "${@/--72mhz/}"

CUSTOM_BOOTLOADER="$PROJECT_DIR/nova_bootloader/.pio/build/$BOOTLOADER_ENV/firmware.bin"
CUSTOM_FW="$PROJECT_DIR/nova_firmware/.pio/build/$FIRMWARE_ENV/firmware.bin"

# Official firmware versions (all CG = Chuck Guard variants)
# Release notes:
#   R2P05x - Original release (May 2018)
#   R2P06E - Increased jam timeout before motor self-stop
#   R2P06K - Latest (current production)
declare -A FW_VERSIONS=(
    ["r2p05x"]="$PROJECT_DIR/firmware/official/firmware_r2p05x_cg.bin"
    ["r2p06e"]="$PROJECT_DIR/firmware/official/firmware_r2p06e_cg.bin"
    ["r2p06k"]="$PROJECT_DIR/firmware/official/firmware_r2p06k_cg.bin"
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
    echo_step "Erasing and flashing via OpenOCD (single session)..."
    scp "$bl_file" "$PI_HOST:/tmp/bootloader.bin"
    scp "$fw_file" "$PI_HOST:/tmp/firmware.bin"

    # Use OpenOCD with 'program' command (faster than write_image)
    # Must keep device halted after unlock to prevent protection re-enabling
    echo_step "Unlocking and programming (OpenOCD session)..."

    # Capture output and check for errors
    local output
    output=$(ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg \
        -c 'transport select swd' \
        -c 'adapter speed 1800' \
        -f target/stm32f1x.cfg \
        -c 'init' \
        -c 'reset halt' \
        -c 'stm32f1x unlock 0' \
        -c 'reset halt' \
        -c 'flash erase_sector 0 0 last' \
        -c 'sleep 100' \
        -c 'program /tmp/bootloader.bin 0x08000000 verify' \
        -c 'sleep 100' \
        -c 'program /tmp/firmware.bin 0x08003000 verify' \
        -c 'reset run' \
        -c 'shutdown'" 2>&1)

    # Show relevant output
    echo "$output" | grep -vE "^(Warn|Debug)" | tail -25

    # Check for errors
    if echo "$output" | grep -q "Programming Failed\|Error:.*flash\|Error:.*write"; then
        echo_err "FLASH PROGRAMMING FAILED!"
        echo_err "Try power cycling the drill press and running again."
        exit 1
    fi
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
        cd "$PROJECT_DIR/nova_bootloader" && pio run -e "$BOOTLOADER_ENV"

        echo_step "Building custom firmware ($FIRMWARE_ENV)..."
        cd "$PROJECT_DIR/nova_firmware" && pio run -e "$FIRMWARE_ENV"

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
        cd "$PROJECT_DIR/nova_firmware" && pio run -e "$FIRMWARE_ENV"
        echo_step "Fast write with st-flash..."
        scp "$CUSTOM_FW" "$PI_HOST:/tmp/firmware.bin"
        ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000"
        echo_step "Resetting MCU..."
        ssh "$PI_HOST" "sudo st-flash reset"
        echo_step "Done!"
        ;;

    quickboot)
        # Flash both bootloader and firmware FAST (assumes unlocked)
        echo_step "Quick flash custom bootloader ($BOOTLOADER_ENV) + firmware ($FIRMWARE_ENV, assumes unlocked)"
        cd "$PROJECT_DIR/nova_bootloader" && pio run -e "$BOOTLOADER_ENV"
        cd "$PROJECT_DIR/nova_firmware" && pio run -e "$FIRMWARE_ENV"
        echo_step "Fast write with st-flash..."
        scp "$CUSTOM_BOOTLOADER" "$PI_HOST:/tmp/bootloader.bin"
        scp "$CUSTOM_FW" "$PI_HOST:/tmp/firmware.bin"
        ssh "$PI_HOST" "sudo st-flash --flash=256k write /tmp/bootloader.bin 0x08000000 && \
            sudo st-flash --flash=256k write /tmp/firmware.bin 0x08003000"
        echo_step "Resetting MCU..."
        ssh "$PI_HOST" "sudo st-flash reset"
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

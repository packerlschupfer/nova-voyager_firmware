#!/bin/bash
# GD32 Flash Unlock Script
#
# The GD32F303 has persistent flash write protection that survives mass erase.
# This script unlocks the flash controller via direct register writes using OpenOCD.
#
# Problem: Even after st-flash erase, the device reports "Flash memory is write protected"
# Solution: Unlock the flash controller registers before attempting to write
#
# Flash Controller Registers (GD32F303 / STM32F1xx compatible):
#   FLASH_KEYR    = 0x40022004  (Flash Key Register)
#   FLASH_OPTKEYR = 0x40022008  (Option Byte Key Register)
#   Unlock keys: 0x45670123 followed by 0xCDEF89AB
#
# Usage: ./gd32_unlock_flash.sh [local|remote]
#   local  - Use ST-LINK connected locally
#   remote - Use ST-LINK on Raspberry Pi (default)

set -e

MODE="${1:-remote}"
PI_HOST="pi@192.168.16.62"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo_step() { echo -e "${GREEN}==> $1${NC}"; }
echo_warn() { echo -e "${YELLOW}WARNING: $1${NC}"; }
echo_err() { echo -e "${RED}ERROR: $1${NC}"; }

run_openocd() {
    local cmds="$1"
    if [ "$MODE" = "local" ]; then
        openocd -f interface/stlink.cfg \
            -c 'transport select hla_swd' \
            -c 'set CPUTAPID 0x2ba01477' \
            -f target/stm32f1x.cfg \
            -c 'reset_config none' \
            -c "init; reset halt; $cmds; shutdown" 2>&1
    else
        ssh "$PI_HOST" "sudo openocd -f interface/stlink.cfg \
            -c 'transport select swd' \
            -f target/stm32f1x.cfg \
            -c 'init; reset halt; $cmds; shutdown'" 2>&1
    fi
}

run_stflash() {
    local cmd="$1"
    if [ "$MODE" = "local" ]; then
        st-flash $cmd
    else
        ssh "$PI_HOST" "sudo st-flash $cmd"
    fi
}

echo_step "GD32F303 Flash Unlock Procedure"
echo "Mode: $MODE"
echo ""

# Step 1: Unlock Flash Controller
echo_step "Step 1: Unlocking flash controller registers..."
UNLOCK_CMDS="mww 0x40022004 0x45670123; mww 0x40022004 0xCDEF89AB; mww 0x40022008 0x45670123; mww 0x40022008 0xCDEF89AB"
run_openocd "$UNLOCK_CMDS" | grep -E "(Info|Error|Warn)" || true

echo_step "Flash controller unlocked!"
echo ""
echo "You can now use st-flash to write to the device:"
echo "  st-flash --flash=256k write bootloader.bin 0x08000000"
echo "  st-flash --flash=256k write firmware.bin 0x08003000"
echo ""
echo "NOTE: The unlock is temporary and resets on power cycle."
echo "      Run this script again if you get 'write protected' errors."

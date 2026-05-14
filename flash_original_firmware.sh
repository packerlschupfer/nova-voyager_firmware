#!/bin/bash
# Flash Original Teknatool R2P06k Firmware
# Usage: ./flash_original_firmware.sh

set -e

PI_HOST="pi@192.168.16.62"
BOOTLOADER="/home/mrnice/Documents/Projects/teknatool_voyager/bootloader_gd32_backup.bin"
FIRMWARE="/home/mrnice/Documents/Projects/teknatool_voyager/firmware_r2p06k_cg_official.bin"

echo "======================================================================"
echo "Flash Original Teknatool R2P06k Firmware"
echo "======================================================================"
echo ""

echo "==> Copying files to Raspberry Pi..."
scp "$BOOTLOADER" "$PI_HOST:/home/pi/bl_orig.bin"
scp "$FIRMWARE" "$PI_HOST:/home/pi/fw_orig.bin"

echo ""
echo "==> Flashing via OpenOCD (handles flash protection)..."
echo "    This may take 30-60 seconds..."
echo ""

ssh "$PI_HOST" 'sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "
  init
  reset halt
  stm32f1x unlock 0
  reset halt
  flash write_image erase /home/pi/bl_orig.bin 0x08000000
  flash write_image erase /home/pi/fw_orig.bin 0x08003000
  verify_image /home/pi/fw_orig.bin 0x08003000
  reset run
  shutdown
" 2>&1' | grep -E "(Info|Error|Warn)" | head -20

echo ""
echo "======================================================================"
echo "✓ Flash Complete!"
echo "======================================================================"
echo ""
echo "Original Teknatool R2P06k firmware restored."
echo ""
echo "Power cycle the Nova Voyager to boot."
echo ""

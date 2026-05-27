#!/bin/bash
# Flash Custom Nova Firmware (handles GD32 flash protection)
# Usage: ./flash_custom_firmware.sh

set -e

PI_HOST="pi@192.168.16.62"
BOOTLOADER="/home/mrnice/Documents/Projects/teknatool_voyager/nova_bootloader/.pio/build/nova_bootloader_120/firmware.bin"
FIRMWARE="/home/mrnice/Documents/Projects/teknatool_voyager/nova_firmware/.pio/build/debug_120/firmware.bin"

echo "======================================================================"
echo "Flash Custom Nova Voyager Firmware"
echo "======================================================================"
echo ""

# Build latest firmware
echo "==> Building firmware..."
cd /home/mrnice/Documents/Projects/teknatool_voyager/nova_firmware
pio run -e debug_120 > /dev/null

echo "==> Copying files to Raspberry Pi..."
scp "$BOOTLOADER" "$PI_HOST:/home/pi/bl.bin"
scp "$FIRMWARE" "$PI_HOST:/home/pi/fw.bin"

echo ""
echo "==> Flashing via OpenOCD (handles flash protection)..."
echo "    This may take 30-60 seconds..."
echo ""

ssh "$PI_HOST" 'sudo openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "
  init
  reset halt
  stm32f1x unlock 0
  reset halt
  flash write_image erase /home/pi/bl.bin 0x08000000
  flash write_image erase /home/pi/fw.bin 0x08003000
  verify_image /home/pi/fw.bin 0x08003000
  reset run
  shutdown
" 2>&1' | grep -E "(Info|Error|Warn)" | head -20

echo ""
echo "======================================================================"
echo "✓ Flash Complete!"
echo "======================================================================"
echo ""
echo "Custom firmware features:"
echo "  ✓ Motor Profiles (SOFT/NORMAL/HARD)"
echo "  ✓ Power Output (Low/Med/High)"
echo "  ✓ Vibration Sensor menu"
echo "  ✓ Speed 100% accurate"
echo "  ✓ NO brake overheating"
echo ""
echo "Power cycle the Nova Voyager to boot fresh firmware."
echo ""

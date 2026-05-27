#!/bin/bash
# Flash Original Teknatool R2P06k Firmware (wrapper)
# Usage: ./flash_original_firmware.sh
#
# The original implementation used a simplified OpenOCD sequence that didn't
# handle GD32 flash protection properly — failures were swallowed and the
# script printed a fake "success". flash_firmware.sh::erase_and_flash_both
# already does this correctly (proper unlock + erase_sector + program verify
# + exit-status checking), so this script is now a thin wrapper.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/flash_firmware.sh" original r2p06k

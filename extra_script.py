"""
PlatformIO Extra Script for Nova Voyager Firmware

Handles:
- Custom linker script
- Custom startup code (required for bootloader compatibility)
- DFU file generation
- Binary size reporting
"""

Import("env")

# Use our custom linker script
env.Replace(LDSCRIPT_PATH="ldscript.ld")

# Exclude framework's startup file - we provide our own
# The bootloader doesn't reinitialize SP, so we need custom startup
def filter_startup(node):
    # Filter out any startup*.s or startup*.S files from framework
    path = node.get_path()
    if "startup_stm32" in path.lower():
        return None
    return node

env.AddBuildMiddleware(filter_startup)

# Post-build: report binary size
def show_size(source, target, env):
    import os
    firmware = str(target[0])
    if os.path.exists(firmware):
        size = os.path.getsize(firmware)
        max_size = 116 * 1024  # 116KB available
        percent = (size / max_size) * 100
        print(f"\n{'='*50}")
        print(f"Firmware size: {size:,} bytes ({percent:.1f}% of {max_size:,})")
        print(f"Free space: {max_size - size:,} bytes")
        print(f"{'='*50}\n")

env.AddPostAction("$BUILD_DIR/firmware.bin", show_size)

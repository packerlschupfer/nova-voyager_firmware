"""
PlatformIO Extra Script for Nova Voyager Firmware

Handles:
- Custom linker script
- Custom startup code (required for bootloader compatibility)
- DFU file generation
- Binary size reporting
"""

Import("env")

#
# Build identity
#
# The firmware used to report a hand-maintained FW_VERSION_STRING in config.h.
# It said "v0.1.0-RTOS" for 35 commits and across ~50 republished release
# builds, so every bug report quoted the same string no matter what was
# actually flashed. Now the version comes from git, which cannot go stale.
#
# Two defines are injected:
#   FW_GIT_DESCRIBE - `git describe --tags --dirty --always`, e.g. "v0.2.0",
#                     "v0.2.0-5-gaf0ca1c" or "v0.2.0-dirty". Absent when there
#                     is no .git (release tarballs), and config.h then falls
#                     back to its own constants - see .gitattributes, which
#                     substitutes the describe output into tarballs.
#   FW_BUILD_ENV    - the PlatformIO env name. Nothing distinguished
#                     nova_voyager from nova_voyager_games or _demo at runtime,
#                     yet they differ in ways that matter when diagnosing: the
#                     games and demo builds carry a 40-sample jam flight
#                     recorder instead of 160, and the demo writes no settings.
#
# Deliberately NOT stamped: __DATE__/__TIME__. Byte-identical rebuilds are a
# property this project checks for (scripts/check-pins.sh exists because a
# release binary once differed from the hardware-validated one), and a
# timestamp in .rodata destroys it. A commit hash does not: it only changes
# when the tree it describes changes.
def _git_describe():
    import subprocess
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always"],
            cwd=env.subst("$PROJECT_DIR"),
            capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return None

_describe = _git_describe()
if _describe:
    env.Append(CPPDEFINES=[("FW_GIT_DESCRIBE", env.StringifyMacro(_describe))])
    print(f"Build identity: {_describe} ({env.subst('$PIOENV')})")
else:
    print(f"Build identity: no git metadata, using config.h constants "
          f"({env.subst('$PIOENV')})")

env.Append(CPPDEFINES=[("FW_BUILD_ENV", env.StringifyMacro(env.subst("$PIOENV")))])

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
        # The application region, as ldscript.ld actually defines it: 244 KB
        # at 0x08003000, less the 2 KB settings page at the top of flash.
        # This said 116 KB for a long time, so every build printed a
        # percentage roughly twice the real one.
        max_size = 242 * 1024
        percent = (size / max_size) * 100
        print(f"\n{'='*50}")
        print(f"Firmware size: {size:,} bytes ({percent:.1f}% of {max_size:,})")
        print(f"Free space: {max_size - size:,} bytes")
        print(f"{'='*50}\n")

env.AddPostAction("$BUILD_DIR/firmware.bin", show_size)

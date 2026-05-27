"""
Post-build script: Create DFU file from firmware binary
"""

Import("env")
import struct
import binascii
import os

def create_dfu(source, target, env):
    """Create DFU file from firmware.bin"""
    # Get the build directory and construct paths
    build_dir = env.subst("$BUILD_DIR")
    firmware_bin = os.path.join(build_dir, "firmware.bin")
    dfu_file = os.path.join(build_dir, "firmware.dfu")

    if not os.path.exists(firmware_bin):
        print(f"Warning: {firmware_bin} not found")
        return

    with open(firmware_bin, 'rb') as f:
        firmware = f.read()

    # DFU file structure
    prefix = b'DfuSe\x01' + struct.pack('<I', 0) + b'\x01'

    # Target prefix
    target_data = b'Target\x00' + struct.pack('<I', 1)
    target_data += b'ST...'.ljust(255, b'\x00')
    target_data += struct.pack('<II', len(firmware) + 8, 1)

    # Element (firmware at 0x08003000)
    element = struct.pack('<II', 0x08003000, len(firmware)) + firmware

    # DFU suffix
    suffix = struct.pack('<HHH', 0, 0, 0x0483)  # Device, Product, Vendor
    suffix += struct.pack('<H', 0x011A) + b'UFD\x10'
    suffix += struct.pack('<I', 0)  # CRC placeholder

    # Combine
    dfu_data = bytearray(prefix + target_data + element + suffix)

    # Update size field
    struct.pack_into('<I', dfu_data, 6, len(dfu_data) - 16)

    # Calculate CRC
    crc = binascii.crc32(dfu_data[:-4]) & 0xFFFFFFFF
    struct.pack_into('<I', dfu_data, len(dfu_data) - 4, crc)

    with open(dfu_file, 'wb') as f:
        f.write(dfu_data)

    print(f"\nDFU file created: {dfu_file} ({len(dfu_data)} bytes)")

# Register post-action
env.AddPostAction("$BUILD_DIR/firmware.bin", create_dfu)

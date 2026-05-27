#!/usr/bin/env python3
"""
Test S0, S7, S8 profiles during motor SPINUP
Measures acceleration time and behavior during startup
"""

import serial
import time
import sys

def send_command_pkt(ser, cmd_chars, param_str=""):
    """Send command in COMMAND format with STX/ETX/checksum"""
    pkt = [0x04, 0x30, 0x30, 0x31, 0x31, 0x02, 0x31]  # Header + STX + '1'
    pkt.extend([ord(c) for c in cmd_chars])
    if param_str:
        pkt.extend([ord(c) for c in param_str])
    pkt.append(0x03)  # ETX

    # Calculate XOR checksum
    xor = 0
    for i in range(6, len(pkt)):
        xor ^= pkt[i]
    pkt.append(xor)

    ser.write(bytes(pkt))
    time.sleep(0.2)

def query_speed(ser):
    """Query actual motor speed"""
    ser.read(ser.in_waiting)
    ser.write(b'QQ SV\r\n')
    time.sleep(0.3)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    for line in response.split('\n'):
        if 'SV' in line and '->' in line:
            hex_part = line.split('->')[1].strip()
            bytes_list = hex_part.split()
            if '02' in bytes_list and '03' in bytes_list:
                stx_idx = bytes_list.index('02')
                etx_idx = bytes_list.index('03')
                data_bytes = bytes_list[stx_idx+1:etx_idx]
                ascii_data = ''.join([chr(int(b, 16)) for b in data_bytes if int(b, 16) >= 0x20])
                if 'SV' in ascii_data:
                    speed_str = ascii_data.split('SV')[1]
                    try:
                        return int(speed_str)
                    except:
                        pass
    return 0

def test_spinup(ser, profile_name, profile_cmd, profile_param, target_rpm=1000):
    """Test motor spinup with a specific profile"""
    print(f"\n{'='*70}")
    print(f"Testing SPINUP: {profile_name} ({profile_cmd})")
    print(f"{'='*70}")

    # Ensure motor is stopped
    ser.write(b'STOP\r\n')
    time.sleep(2)

    # Set profile
    print(f"  Setting profile {profile_cmd}({profile_param})...")
    send_command_pkt(ser, profile_cmd, profile_param)
    time.sleep(0.5)

    # Set speed
    ser.write(f'SPEED {target_rpm}\r\n'.encode())
    time.sleep(0.3)

    # Record start time and start motor
    print(f"  Starting motor to {target_rpm} RPM...")
    print(f"  WATCH & LISTEN: How does it accelerate?")
    print()

    start_time = time.time()
    ser.write(b'START\r\n')

    # Monitor spinup
    speeds = []
    timestamps = []

    for i in range(15):  # Monitor for 15 seconds
        elapsed = time.time() - start_time
        speed = query_speed(ser)

        speeds.append(speed)
        timestamps.append(elapsed)

        status = "ACCELERATING" if speed < target_rpm * 0.95 else "AT SPEED"
        print(f"    [{elapsed:4.1f}s] Speed: {speed:4d} RPM  ({status})")

        if speed >= target_rpm * 0.95 and len(speeds) > 3:
            # Reached target speed
            break

        time.sleep(0.5)

    # Calculate spinup time
    spinup_time = None
    for i, speed in enumerate(speeds):
        if speed >= target_rpm * 0.95:
            spinup_time = timestamps[i]
            break

    print()
    print(f"  Results:")
    if spinup_time:
        print(f"    Time to reach {target_rpm} RPM: {spinup_time:.1f} seconds")
    else:
        print(f"    Did not reach {target_rpm} RPM in test period")
    print(f"    Final speed: {speeds[-1]} RPM")

    return {
        'profile': profile_name,
        'spinup_time': spinup_time,
        'speeds': speeds,
        'timestamps': timestamps,
        'final_speed': speeds[-1]
    }

def main():
    print("="*70)
    print("MOTOR PROFILE SPINUP TEST")
    print("="*70)
    print()
    print("This test will:")
    print("  1. Stop motor")
    print("  2. Set profile S0 (SOFT) and START - WATCH acceleration")
    print("  3. Stop motor")
    print("  4. Set profile S7 (NORMAL) and START - WATCH acceleration")
    print("  5. Stop motor")
    print("  6. Set profile S8 (HARD) and START - WATCH acceleration")
    print("  7. Compare spinup times")
    print()
    print("OBSERVE during each spinup:")
    print("  - How fast does motor accelerate?")
    print("  - Smooth or jerky?")
    print("  - Loud or quiet?")
    print("  - Does it sound different between profiles?")
    print()

    if sys.stdin.isatty():
        input("Press Enter to start (or Ctrl+C to cancel)...")
    else:
        print("Auto-starting in 3 seconds...")
        time.sleep(3)

    # Open serial port
    ser = serial.Serial('/dev/ttyUSB0', 9600, timeout=2)
    time.sleep(1)
    ser.read(ser.in_waiting)

    try:
        results = []

        # Test S0 (SOFT)
        results.append(test_spinup(ser, "SOFT (S0)", "S0", "0"))
        time.sleep(3)

        # Test S7 (NORMAL)
        results.append(test_spinup(ser, "NORMAL (S7)", "S7", "750"))
        time.sleep(3)

        # Test S8 (HARD)
        results.append(test_spinup(ser, "HARD (S8)", "S8", "264"))
        time.sleep(3)

        # Stop motor
        print("\nStopping motor...")
        ser.write(b'STOP\r\n')
        time.sleep(2)

        # Print comparison
        print("\n" + "="*70)
        print("SPINUP COMPARISON")
        print("="*70)
        print()
        print(f"{'Profile':<20} {'Spinup Time':<15} {'Final Speed':<15}")
        print("-"*70)

        for r in results:
            spinup_str = f"{r['spinup_time']:.1f}s" if r['spinup_time'] else "N/A"
            print(f"{r['profile']:<20} {spinup_str:<15} {r['final_speed']:<15} RPM")

        print()
        print("ANALYSIS:")
        print("-"*70)

        # Check if profiles made a difference
        valid_times = [r['spinup_time'] for r in results if r['spinup_time']]

        if len(valid_times) >= 2:
            fastest = min(valid_times)
            slowest = max(valid_times)
            diff = slowest - fastest

            print(f"Fastest spinup: {fastest:.1f}s")
            print(f"Slowest spinup: {slowest:.1f}s")
            print(f"Difference: {diff:.1f}s")
            print()

            if diff > 0.5:
                print(f"✓ SIGNIFICANT DIFFERENCE: {diff:.1f}s spinup time difference!")
                print("  Profiles DO affect motor acceleration!")
                print()
                # Find which was fastest/slowest
                for r in results:
                    if r['spinup_time'] == fastest:
                        print(f"  FASTEST: {r['profile']} ({r['spinup_time']:.1f}s)")
                    if r['spinup_time'] == slowest:
                        print(f"  SLOWEST: {r['profile']} ({r['spinup_time']:.1f}s)")
            else:
                print("✗ No significant difference in spinup times")
                print("  Profiles may not affect acceleration")
        else:
            print("Not enough valid measurements to compare")

        print()
        print("Did you HEAR or SEE differences during spinup? (y/n)")

    except KeyboardInterrupt:
        print("\n\nTest interrupted by user!")
        ser.write(b'STOP\r\n')
    except Exception as e:
        print(f"\n\nERROR: {e}")
        import traceback
        traceback.print_exc()
        ser.write(b'STOP\r\n')
    finally:
        ser.close()
        print("\nSerial port closed.")

if __name__ == '__main__':
    main()

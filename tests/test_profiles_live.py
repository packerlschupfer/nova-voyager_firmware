#!/usr/bin/env python3
"""
Test S0, S7, S8 profile commands with motor running
Tests if profiles affect motor behavior (torque response, speed regulation)
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
    time.sleep(0.1)

def query_speed(ser):
    """Query actual motor speed"""
    ser.read(ser.in_waiting)
    ser.write(b'QQ SV\r\n')
    time.sleep(0.5)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    # Extract speed from response like "Query format: SV -> 02 31 53 56 31 30 30 30 03"
    # which means "1SV1000"
    for line in response.split('\n'):
        if 'SV' in line and '->' in line:
            # Parse hex bytes
            hex_part = line.split('->')[1].strip()
            bytes_list = hex_part.split()
            if '02' in bytes_list and '03' in bytes_list:
                stx_idx = bytes_list.index('02')
                etx_idx = bytes_list.index('03')
                data_bytes = bytes_list[stx_idx+1:etx_idx]
                ascii_data = ''.join([chr(int(b, 16)) for b in data_bytes if int(b, 16) >= 0x20])
                # Extract number from "1SV1000" format
                if 'SV' in ascii_data:
                    speed_str = ascii_data.split('SV')[1]
                    try:
                        return int(speed_str)
                    except:
                        pass
    return None

def query_load(ser):
    """Get calculated load from firmware"""
    ser.read(ser.in_waiting)
    ser.write(b'STATUS\r\n')
    time.sleep(0.5)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    for line in response.split('\n'):
        if 'Load:' in line:
            try:
                load = int(line.split(':')[1].strip().replace('%', ''))
                return load
            except:
                pass
    return None

def test_profile(ser, profile_name, profile_cmd, profile_param):
    """Test a profile and monitor motor response"""
    print(f"\n{'='*70}")
    print(f"Testing Profile: {profile_name} ({profile_cmd})")
    print(f"{'='*70}")

    # Send profile command
    print(f"  Sending {profile_cmd}({profile_param})...")
    send_command_pkt(ser, profile_cmd, profile_param)
    time.sleep(0.5)

    # Monitor for 10 seconds
    print(f"  Monitoring for 10 seconds...")
    speeds = []
    loads = []

    for i in range(10):
        speed = query_speed(ser)
        load = query_load(ser)

        if speed is not None:
            speeds.append(speed)
        if load is not None:
            loads.append(load)

        print(f"    [{i+1:2d}s] Speed: {speed if speed else 'N/A':>4} RPM, Load: {load if load else 'N/A':>3}%")
        time.sleep(1)

    # Calculate statistics
    avg_speed = sum(speeds) / len(speeds) if speeds else 0
    avg_load = sum(loads) / len(loads) if loads else 0
    speed_variation = max(speeds) - min(speeds) if speeds else 0

    print(f"\n  Results:")
    print(f"    Average Speed:     {avg_speed:.1f} RPM")
    print(f"    Speed Variation:   {speed_variation} RPM (max - min)")
    print(f"    Average Load:      {avg_load:.1f}%")

    return {
        'profile': profile_name,
        'avg_speed': avg_speed,
        'speed_var': speed_variation,
        'avg_load': avg_load,
        'speeds': speeds,
        'loads': loads
    }

def main():
    print("="*70)
    print("MOTOR PROFILE BEHAVIOR TEST")
    print("="*70)
    print()
    print("This test will:")
    print("  1. Start motor at 1000 RPM")
    print("  2. Test S0 (SOFT profile)")
    print("  3. Test S7 (NORMAL profile)")
    print("  4. Test S8 (HARD profile)")
    print("  5. Compare motor behavior between profiles")
    print()
    print("SAFETY:")
    print("  - Guard must be CLOSED")
    print("  - E-stop within reach")
    print("  - No load on spindle (free spinning)")
    print()

    if sys.stdin.isatty():
        input("Press Enter to start test (or Ctrl+C to cancel)...")
    else:
        print("Auto-starting in 3 seconds...")
        time.sleep(3)

    # Open serial port
    ser = serial.Serial('/dev/ttyUSB0', 9600, timeout=2)
    time.sleep(1)
    ser.read(ser.in_waiting)

    try:
        # Start motor at 1000 RPM
        print("\nStarting motor at 1000 RPM...")
        ser.write(b'SPEED 1000\r\n')
        time.sleep(0.5)
        ser.write(b'START\r\n')
        time.sleep(3)

        # Verify motor is running
        speed = query_speed(ser)
        if speed is None or speed < 500:
            print(f"ERROR: Motor not running properly (speed={speed})")
            ser.write(b'STOP\r\n')
            return

        print(f"Motor running at {speed} RPM\n")
        time.sleep(2)

        # Test each profile
        results = []

        # Test S0 (SOFT)
        results.append(test_profile(ser, "SOFT (S0)", "S0", "0"))
        time.sleep(3)

        # Test S7 (NORMAL)
        results.append(test_profile(ser, "NORMAL (S7)", "S7", "750"))
        time.sleep(3)

        # Test S8 (HARD)
        results.append(test_profile(ser, "HARD (S8)", "S8", "264"))
        time.sleep(3)

        # Stop motor
        print("\nStopping motor...")
        ser.write(b'STOP\r\n')
        time.sleep(2)

        # Print comparison
        print("\n" + "="*70)
        print("COMPARISON SUMMARY")
        print("="*70)
        print()
        print(f"{'Profile':<15} {'Avg Speed':<12} {'Speed Var':<12} {'Avg Load':<12}")
        print("-"*70)

        for r in results:
            print(f"{r['profile']:<15} {r['avg_speed']:>8.1f} RPM {r['speed_var']:>8} RPM {r['avg_load']:>8.1f}%")

        print()
        print("ANALYSIS:")
        print("-"*70)

        # Check if profiles made a difference
        speeds_diff = max([r['avg_speed'] for r in results]) - min([r['avg_speed'] for r in results])
        loads_diff = max([r['avg_load'] for r in results]) - min([r['avg_load'] for r in results])

        if speeds_diff < 20:
            print("✗ Speed regulation: NO significant difference between profiles")
        else:
            print(f"✓ Speed regulation: {speeds_diff:.1f} RPM difference detected!")

        if loads_diff < 5:
            print("✗ Load response: NO significant difference between profiles")
        else:
            print(f"✓ Load response: {loads_diff:.1f}% difference detected!")

        print()

        if speeds_diff < 20 and loads_diff < 5:
            print("CONCLUSION: Profile commands do NOT affect motor behavior.")
            print("            Profiles may be read-only status registers,")
            print("            or require motor restart to take effect.")
        else:
            print("CONCLUSION: Profile commands DO affect motor behavior!")
            print("            Implement profile selection menu in firmware.")

    except KeyboardInterrupt:
        print("\n\nTest interrupted by user!")
        ser.write(b'STOP\r\n')
    except Exception as e:
        print(f"\n\nERROR: {e}")
        ser.write(b'STOP\r\n')
    finally:
        ser.close()
        print("\nSerial port closed.")

if __name__ == '__main__':
    main()

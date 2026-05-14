#!/usr/bin/env python3
"""Serial command helper - properly closes port to avoid FD leaks"""
import sys
import serial
import time

PORT = "/dev/ttyNova"
BAUD = 9600

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else None
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

    try:
        with serial.Serial(PORT, BAUD, timeout=0.1) as ser:
            # Flush any pending data
            ser.reset_input_buffer()
            time.sleep(0.05)

            if cmd:
                # Send command char by char with small delay (like typing)
                for c in cmd:
                    ser.write(c.encode())
                    time.sleep(0.02)  # 20ms between chars
                ser.write(b'\r')
                time.sleep(0.5)  # Wait for processing

            # Read all available data with longer collection
            end_time = time.time() + timeout
            output = ""
            idle_count = 0
            while time.time() < end_time:
                data = ser.read(512)
                if data:
                    output += data.decode('utf-8', errors='replace')
                    idle_count = 0
                else:
                    idle_count += 1
                    if idle_count > 10 and output:  # 1 sec idle after data
                        break
                    time.sleep(0.1)

            # Strip the echo of our command from start
            if cmd and output.startswith(cmd):
                output = output[len(cmd):]

            print(output.strip())
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()

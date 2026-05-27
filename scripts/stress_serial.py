#!/usr/bin/env python3
"""Hammer the console with commands whose reply is exactly predictable.
Any corruption shows up as a mismatch or an "Unknown:" reply."""
import serial, sys, time

n = int(sys.argv[1]); gap = float(sys.argv[2])
bad = 0; unknown = 0
with serial.Serial('/dev/ttyNova', 115200, timeout=0.4) as ser:
    # Settle after open, then flush. Opening a CH340 toggles DTR/RTS, which
    # glitches the line and injects one spurious byte into the firmware's RX.
    # A bare CR discards whatever partial line that left in its command buffer.
    time.sleep(0.15)
    ser.write(b'\r')
    time.sleep(0.15)
    ser.reset_input_buffer()
    for i in range(n):
        val = 50 + (i % 451)
        cmd = f"TAPSET chipms {val}"
        ser.write((cmd + '\r').encode())
        time.sleep(gap)
        got = ser.read(4096).decode('ascii', 'replace')
        if f"OK chipms={val}" not in got:
            bad += 1
            if "Unknown" in got: unknown += 1
            print(f"MISMATCH #{i} sent={cmd!r} got={got!r}", flush=True)
print(f"sent={n} mismatches={bad} unknown_replies={unknown}")

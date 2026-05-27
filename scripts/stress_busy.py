#!/usr/bin/env python3
"""Console stress WHILE the machine is busy.

An idle link proves little: the risk at high baud is USART1 (NVIC prio 6)
being starved inside FreeRTOS critical sections. So this runs a real tapping
cycle -- motor UART polling, LCD repaints, direction-change command bursts --
and hammers the console throughout, verifying every reply.
"""
import serial, sys, time

n = int(sys.argv[1]); gap = float(sys.argv[2])
bad = unknown = 0

def send(ser, cmd, settle=0.25):
    ser.write((cmd + '\r').encode()); time.sleep(settle)
    return ser.read(8192).decode('ascii', 'replace')

with serial.Serial('/dev/ttyNova', 115200, timeout=0.2) as ser:
    time.sleep(0.15); ser.write(b'\r'); time.sleep(0.15); ser.reset_input_buffer()
    for c in ("TAPSET pedal 1", "TAPSET pedact 1", "TAPSIM P0", "ARM 1"):
        send(ser, c)
    send(ser, "TAPTEST", 0.4)
    print("spindle running; hammering console", flush=True)

    for i in range(n):
        val = 50 + (i % 451)
        got = send(ser, f"TAPSET chipms {val}", gap)
        if f"OK chipms={val}" not in got:
            bad += 1
            if "Unknown" in got: unknown += 1
            print(f"MISMATCH #{i} got={got!r}", flush=True)
        # periodic chip break: worst case for ISR latency
        if i and i % 25 == 0:
            send(ser, "TAPSIM P1", 0.35)
            send(ser, "TAPSIM P0", 0.35)

    send(ser, "TAPSTOP", 0.4)
print(f"sent={n} mismatches={bad} unknown_replies={unknown}")

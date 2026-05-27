#!/usr/bin/env python3
"""Drive a timed command sequence on the Nova console and log everything.

Usage: tap_drive.py <total_seconds> <t1>:<cmd1> <t2>:<cmd2> ...
One port, one owner: sending and logging have to share the link, so a
separate capture process cannot be used.
"""
import serial, sys, time

dur = float(sys.argv[1])
sched = []
for a in sys.argv[2:]:
    t, cmd = a.split(':', 1)
    sched.append((float(t), cmd))
sched.sort()

with serial.Serial('/dev/ttyNova', 115200, timeout=0.05) as ser:
    # See stress_serial.py: opening a CH340 toggles DTR/RTS and injects one
    # spurious byte into the firmware's RX. Settle, flush it with a bare CR,
    # then start. Without this the FIRST command of a run is silently corrupted
    # (observed as "TTAPSET"/"STAPSET"), which invalidates the whole run.
    time.sleep(0.15)
    ser.write(b'\r')
    time.sleep(0.15)
    ser.reset_input_buffer()
    t0 = time.time()
    buf = b''
    nxt = 0
    while True:
        now = time.time() - t0
        if now >= dur:
            break
        while nxt < len(sched) and now >= sched[nxt][0]:
            cmd = sched[nxt][1]
            ser.write((cmd + '\r').encode())
            print(f"+{now:6.3f}  >>> {cmd}", flush=True)
            nxt += 1
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                txt = line.decode('ascii', 'replace').strip()
                if txt and txt != '>':
                    print(f"+{time.time()-t0:6.3f}  {txt}", flush=True)

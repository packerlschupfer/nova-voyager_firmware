#!/usr/bin/env python3
"""Poll VIBRAW continuously and report EXCURSIONS, not averages.

With no accelerometer fitted the I2C bus floats: every data register reads
0xFF, which sign-extends to -1, giving X=-5 Y=245 Z=-5 (Y carries the OEM's
+250 offset). At MAX sensitivity the OEM warns at >250 - so Y sits SIX counts
under its own alarm threshold. The open question is whether motor EMI ever
flips a bit on that floating bus and pushes a single read over the line.

A mean shift would not show that. One bad read in thousands would. So this
records the extremes and counts anything that would have tripped the alarm.
"""
import sys, time, re, serial

PORT, BAUD = "/dev/ttyNova", 115200
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
WARN = 251   # OEM level-1 threshold at sensitivity 3 (fires on >250)

pat = re.compile(rb'X=\s*(-?\d+)\s+Y=\s*(-?\d+)\s+Z=\s*(-?\d+)')
raw = re.compile(rb'raw regs 1\.\.6:\s*(.*)')

lo = [10**9]*3; hi = [-10**9]*3
n = 0; trips = 0; rawset = {}
t0 = time.time()
with serial.Serial(PORT, BAUD, timeout=0.05) as ser:
    time.sleep(2); ser.write(b'\r\n'); time.sleep(0.3); ser.reset_input_buffer()
    while time.time() - t0 < SECONDS:
        ser.write(b'VIBRAW\r\n')
        buf = b''
        deadline = time.time() + 0.25
        while time.time() < deadline:
            chunk = ser.read(512)
            if chunk:
                buf += chunk
                if b'Z=' in buf: break
            elif buf:
                break
        m = pat.search(buf)
        if not m: continue
        v = [int(m.group(i)) for i in (1,2,3)]
        n += 1
        for i in range(3):
            lo[i] = min(lo[i], v[i]); hi[i] = max(hi[i], v[i])
        r = raw.search(buf)
        if r: rawset[r.group(1).strip()] = rawset.get(r.group(1).strip(), 0) + 1
        if max(abs(v[0]), abs(v[1]), abs(v[2])) >= WARN and v[1] != 245:
            trips += 1
            print('TRIP  X=%d Y=%d Z=%d' % tuple(v), flush=True)
        elif v != [-5, 245, -5]:
            print('MOVE  X=%d Y=%d Z=%d' % tuple(v), flush=True)

print()
print('samples      : %d over %.0fs' % (n, time.time()-t0))
print('X range      : %d .. %d' % (lo[0], hi[0]))
print('Y range      : %d .. %d   (alarm at >=%d)' % (lo[1], hi[1], WARN))
print('Z range      : %d .. %d' % (lo[2], hi[2]))
print('would-trip   : %d' % trips)
for k, c in sorted(rawset.items(), key=lambda kv: -kv[1]):
    print('raw regs     : %s  x%d' % (k.decode(errors='replace'), c))

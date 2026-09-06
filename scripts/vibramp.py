#!/usr/bin/env python3
"""Ramp the spindle while sampling VIBRAW, looking for EMI-induced excursions.

One serial owner: the ramp and the sampling share a connection, so nothing
races for the port. STOP is in a finally block and runs on any exit path.
"""
import time, re, sys, serial

PORT, BAUD = "/dev/ttyNova", 115200
STEPS = [2500, 4500, 5500]
DWELL = 22.0          # seconds at each speed
WARN  = 251           # OEM level-1 threshold at sensitivity 3 (fires on >250)
BASE  = [-5, 245, -5] # floating-bus signature

import os
SLOW = int(os.environ.get('SLOW','1'))
SAMPLE_CMD = (('VIBRAW %d'%SLOW) if SLOW>1 else 'VIBRAW').encode()+b'\r\n'
pat = re.compile(rb'X=\s*(-?\d+)\s+Y=\s*(-?\d+)\s+Z=\s*(-?\d+)')

lo=[10**9]*3; hi=[-10**9]*3
n=0; trips=0; moves=0; per={}

def send(ser, cmd):
    ser.write(cmd.encode()+b'\r\n'); time.sleep(0.25); ser.reset_input_buffer()

def sample(ser):
    global n, trips, moves
    ser.write(SAMPLE_CMD)
    buf=b''; deadline=time.time()+0.25
    while time.time()<deadline:
        c=ser.read(512)
        if c:
            buf+=c
            if b'Z=' in buf: break
        elif buf: break
    m=pat.search(buf)
    if not m: return None
    v=[int(m.group(i)) for i in (1,2,3)]
    n+=1
    for i in range(3):
        lo[i]=min(lo[i],v[i]); hi[i]=max(hi[i],v[i])
    if max(abs(v[0]),abs(v[1]),abs(v[2]))>=WARN and v[1]!=245:
        trips+=1; print('  TRIP  X=%d Y=%d Z=%d'%tuple(v), flush=True)
    elif v!=BASE:
        moves+=1; print('  MOVE  X=%d Y=%d Z=%d'%tuple(v), flush=True)
    return v

ser=serial.Serial(PORT,BAUD,timeout=0.05)
try:
    time.sleep(2); ser.write(b'\r\n'); time.sleep(0.3); ser.reset_input_buffer()
    print('START', flush=True)
    send(ser,'SPEED %d'%STEPS[0]); send(ser,'START'); time.sleep(3)
    for rpm in STEPS:
        print('--- %d RPM ---'%rpm, flush=True)
        send(ser,'SPEED %d'%rpm)
        time.sleep(2.5)                       # let it reach speed
        c0=n; t0=time.time()
        while time.time()-t0 < DWELL:
            sample(ser)
        per[rpm]=n-c0
finally:
    try:
        ser.write(b'STOP\r\n'); time.sleep(1.0)
        ser.write(b'STOP\r\n'); time.sleep(1.0)
        print('STOP sent', flush=True)
    finally:
        ser.close()

print()
print('samples    : %d'%n)
print('per speed  : %s'%', '.join('%d:%d'%(k,v) for k,v in per.items()))
print('X range    : %d .. %d'%(lo[0],hi[0]))
print('Y range    : %d .. %d   (alarm at >=%d)'%(lo[1],hi[1],WARN))
print('Z range    : %d .. %d'%(lo[2],hi[2]))
print('deviations : %d'%moves)
print('would-trip : %d'%trips)

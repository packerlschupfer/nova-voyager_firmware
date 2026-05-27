# Sensor Initialization - Final Conclusions

**Date:** January 17, 2026  
**Status:** RESOLVED - Sensor init NOT needed, MCB uses EEPROM defaults

---

## Executive Summary

After extensive testing and analysis, **sensor initialization commands should NOT be sent during boot**. The MCB motor controller has factory defaults stored in persistent EEPROM that work perfectly. Any initialization attempts (CL query, sensor commands, or status queries) break motor operation.

---

## What Breaks Motor Operation

**All of the following prevent motor from starting after cold boot:**

1. **CL Query** - `send_query(CMD_CURRENT_LIMIT)`
   - Leaves MCB in sensor/config mode
   - Motor won't accept ST/JF commands

2. **Sensor Init Commands** - VR(0), CL(0), VS(0), T0(0), TH(60)
   - Even with correct sequence from original firmware
   - Spindle mechanically FREE but motor won't start
   - Requires soft reset to recover

3. **Initial Status Query** - motor_query_status() + motor_query_temperature()
   - Breaks motor even after successful motor_sync
   - Motor won't start until soft reset

4. **Boot Complete Beep** - From motor task context
   - Causes hard fault (HFSR=0x40000000)
   - GPIO timing issue from motor task

---

## Final Working Configuration

**Boot Sequence:**
```c
1. Wait 50ms for MCB ready (all boot types)
2. motor_sync_settings() - Send factory parameters
3. Enter main loop
4. Status queries happen during normal operation
```

**Boot Time:** 750ms (0.75 seconds)

**What's NOT Done:**
- ❌ No CL query
- ❌ No sensor initialization
- ❌ No initial status query
- ❌ No boot beeps

**What Works:**
- ✅ Motor starts immediately on all boot types
- ✅ Temperature readable via T0 query (TEMP command)
- ✅ Vibration configurable via menu (VG runtime setting)
- ✅ MCB autonomous thermal protection (via GF flags)
- ✅ All 6 tapping modes functional

---

## MCB EEPROM Persistence

**The MCB has persistent EEPROM** containing:
- Factory default sensor parameters (V8, VG, T0, TH)
- Motor calibration (IR gain/offset)
- Thermal thresholds
- Vibration settings

**These values:**
- Persist across power cycles
- Work perfectly without reinitialization
- Were set by original Teknatool firmware
- Should NOT be overwritten during normal boot

---

## Temperature Monitoring

### T0 Command - Dual Purpose

**T0 has two completely different functions:**

1. **T0(0) as COMMAND:** Sets thermal baseline
   - Sent during sensor init
   - **Breaks motor operation!**
   - Should NOT be sent

2. **T0 as QUERY:** Reads current MCB heatsink temperature
   - Returns actual temperature (e.g., "T018" = 18°C)
   - Works WITHOUT sending T0(0) command first
   - Safe to query anytime
   - Used by TEMP serial command

### HT Command - Returns Threshold

**HT query returns configured threshold, NOT measured temperature:**
- HT response: "HT75" = 75°C (the TH threshold we would set)
- NOT useful for temperature monitoring
- Original firmware uses T0 query, not HT

### Dual Temperature Display

**TEMP command shows both:**
- MCB heatsink: 17-18°C (from T0 query)
- HMI GD32 chip: 27-31°C (internal sensor)

---

## Boot Type Behavior

### COLD BOOT (Power-On)
- 50ms MCB wait
- motor_sync_settings
- ~750ms total
- **Motor works!** ✅

### WATCHDOG RESET (After Flash)
- 50ms MCB wait (was 200ms, not needed)
- motor_sync_settings
- ~750ms total
- **Motor works!** ✅

### SOFT BOOT (OFF Button)
- 50ms MCB wait
- motor_sync_settings
- ~520ms total (slightly faster)
- **Motor works!** ✅

**All boot types behave identically - reliable!**

---

## Why Original Firmware Sensor Init Works

**Hypothesis:** Original firmware only runs sensor init on **FIRST BOOT** (factory setup):
- User configures language, units, precision
- Sensor init runs ONCE
- Flag stored in flash to skip on subsequent boots
- Normal boots skip sensor init entirely

**Evidence:**
- Conditional vibration init call at 0x800afa2 (checks flag)
- "Factory Reset" strings in firmware
- Our testing never verified motor operation with sensor init
- MCB EEPROM has persistent defaults

---

## Optimization Results

### Boot Time Progression

| Version | Time | vs Baseline |
|---------|------|-------------|
| With sensor init (broken motor) | 3200ms | Baseline |
| No sensor init, 200ms wait | 1367ms | -57% |
| 5ms delays, 100ms wait | 915ms | -33% |
| 5ms delays, 50ms wait | 780ms | -15% |
| **Final (no status query)** | **750ms** | **-4%** ✅ |
| **TOTAL IMPROVEMENT** | | **-77%** 🚀 |

### Optimizations Applied

1. ✅ Sensor init removed (breaks motor)
2. ✅ Boot beeps disabled (saves 450ms)
3. ✅ motor_sync delays: 50ms → 5ms (match original)
4. ✅ MCB wait: 200ms → 50ms
5. ✅ Status query removed (breaks motor)

---

## Commands That Work Without Init

**These work WITHOUT sending init commands first:**

| Command | Purpose | Works? |
|---------|---------|--------|
| **T0 query** | Read MCB heatsink temp | ✅ YES |
| **GF query** | Read motor status flags | ✅ YES |
| **SV query** | Read motor speed | ✅ YES |
| **VG command** | Set vibration sensitivity | ✅ YES (runtime) |
| **ST/JF/RS** | Motor control | ✅ YES |

**MCB responds to all commands using EEPROM defaults.**

---

## Final Recommendations

**For Production:**
1. ✅ Keep sensor init DISABLED
2. ✅ Use 50ms MCB wait (all boot types)
3. ✅ Skip initial status query
4. ✅ Query status only during motor operation
5. ✅ Silent boot (no beeps from motor task)

**For Future Investigation:**
- Analyze original firmware first-boot flag mechanism
- Understand when sensor init is appropriate (factory reset only?)
- Consider implementing "factory reset" feature that runs sensor init once

---

## Testing Protocol

**To verify motor operation after any boot change:**

1. ✅ Power cycle (full power off/on)
2. ✅ Wait for "Motor init complete"
3. ✅ **Press START button immediately**
4. ✅ Motor MUST spin (not just display showing FWD)
5. ✅ Test on all boot types (COLD/WATCHDOG/SOFT)

**Previously we only tested:**
- ❌ Spindle manual rotation (not sufficient!)
- ❌ Assumed FREE spindle = motor works (wrong!)

**Correct test:**
- ✅ Actual motor START command
- ✅ Verify motor spins

---

## Conclusion

**MCB initialization is NOT required.** The MCB has factory defaults in EEPROM that work perfectly. Boot sequence should only sync motor parameters (IR comp, PID values, etc.) and immediately enter operation mode.

**Current firmware:**
- Boot time: 750ms ✅
- Motor works on all boot types ✅
- All features functional ✅
- Production ready ✅

---

END OF SENSOR INITIALIZATION ANALYSIS

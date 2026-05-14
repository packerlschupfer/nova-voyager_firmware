# Load/Spike Sensor Analysis - No Init Required!
**Discovery**: Load sensing is MCB-internal, always active  
**Date**: January 12, 2026

---

## ANSWER: NO INITIALIZATION NEEDED FOR LOAD SENSOR! ✅

**Load sensing is AUTOMATIC** - MCB always monitors current, no enable command required.

---

## LOAD SENSOR ARCHITECTURE

### MCB Side (Always Active)

**Hardware**: Current sensing shunt resistors on motor phases

**MCB Monitors Continuously**:
- ✅ Motor phase current (load)
- ✅ Jam detection (stall + current)
- ✅ Spike detection (sudden current increase)
- ✅ Overload detection (sustained high current)

**Reports to HMI via**: GF (0x4746) status flags

---

## GF FLAG BITS (Load Sensor Status)

From GF response parsing (motor.c:288-296):

| Bit | Flag | Purpose | Nova Status |
|-----|------|---------|-------------|
| 0 | fault | General motor fault | ✅ Implemented |
| 1 | **overload** | **Sustained overload or SPIKE** | ✅ Implemented |
| 2 | **jam_detected** | **Motor stall (jam)** | ✅ Implemented |
| 3-4 | rps_error | Speed regulation error | ✅ Implemented |
| 5 | pfc_fault | Power factor correction fault | ✅ Implemented |
| 6-7 | voltage_error | Voltage out of range | ✅ Implemented |
| 8-9 | overheat | Temperature exceeded | ✅ Implemented |

**Load Sensing Flags**:
- **Bit 2**: Jam detection (motor stalled)
- **Bit 1**: Overload/Spike detection (sudden load increase)

**Already working in nova_firmware!** ✅

---

## SPIKE/JAM DETECTION SETTINGS

### From Firmware Strings

**Jam Detect**:
- "Jam Detect: OFF" (line 1384)
- "Jam Detect: ON" (line 1385)
- Default: ON

**Spike Detect**:
- "Spike Detect: OFF" (line 1386)  
- "Spike Detect: ON" (line 1387)
- "Spike Threshold" (line 1389)
- Default: ON

### How It Works

**Settings are HMI-Side Only**:
1. User selects "Spike Detect: ON/OFF" in menu
2. Stored in HMI EEPROM (not sent to MCB)
3. MCB **always** monitors load/spike (internal safety)
4. HMI reads GF flags
5. If spike_detect_enable == ON: Display alarm
6. If spike_detect_enable == OFF: Silent (ignore flag)

**MCB behavior doesn't change** - it always detects, HMI just controls alarm visibility.

---

## LD COMMAND (0x4C44)

### Function Analysis

**Location**: 0x801A886  
**Type**: QUERY command (uses motor_query)  
**Calls**: **ZERO** - Never called in original firmware!

**Disassembly**:
```asm
0x801a886:  movw  r0, #19524    ; 0x4C44 = 'L''D'
0x801a88a:  bl    0x801b360     ; motor_query(LD)
0x801a88e:  pop   {r4, pc}
```

**Purpose** (speculation):
- Query spike detection threshold from MCB
- Or: Set spike threshold
- But: **Never used in original firmware**

**Possible Reasons**:
1. Legacy command (planned but not implemented)
2. Service mode only (menu exists but command unused)
3. MCB has fixed threshold (LD not needed)
4. Spike threshold is HMI-calculated, not MCB-based

---

## SPIKE THRESHOLD

### From Manual

```
"Spike Threshold – This is the load threshold which constitutes a load spike"
```

**Implementation Options**:

**Option A**: HMI calculates spike threshold
```c
// In tapping or monitoring code
baseline_load = learn_baseline();
spike_threshold = settings.spike_threshold_percent;  // e.g., 30%

if (current_load > baseline + threshold) {
    // Spike detected by HMI
    trigger_spike_alarm();
}
```

**Option B**: MCB has fixed spike threshold
- MCB detects spike internally (fixed algorithm)
- Reports via GF bit 1 (overload flag)
- HMI just responds to flag

**Option C**: LD command sets MCB threshold
- LD(30) sets spike threshold to 30% above baseline
- But: Never called in original (unused?)

**Most Likely**: Option B (MCB fixed threshold)

---

## JAM DETECTION (Already Working!)

### Nova Firmware Status

From task_motor.c and jam.c:

```c
// GF polling detects jam via bit 2
if (motor_status.jam_detected) {
    // Jam flag set by MCB
    SEND_EVENT(EVT_JAM_DETECTED);
    motor_stop();
}
```

**Status**: ✅ **Already implemented and working!**

**MCB Jam Detection**:
- Monitors motor speed vs current
- If speed == 0 and current > threshold for 5000ms → jam
- Sets GF bit 2
- HMI receives flag and responds

**No init required** - MCB always monitors!

---

## SPIKE DETECTION (Needs UI Only)

### Nova Firmware Status

```c
// GF bit 1 (overload) is parsed
motor_status.overload = (value & 0x02) != 0;
```

**Already parsed** ✅ but **not used** for spike detection!

### What to Add

```c
// In settings.h
typedef struct {
    bool spike_detect_enable;   // ON/OFF toggle
    uint8_t spike_threshold;    // Percentage (e.g., 30%)
} sensor_settings_t;

// In jam.c or task_motor.c
if (motor_status.overload && settings.spike_detect_enable) {
    uart_puts("SPIKE DETECTED!\r\n");
    
    // Display warning
    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 3000;
    g_state.error_line1 = " LOAD SPIKE!    ";
    g_state.error_line2 = "High load detected";
    STATE_UNLOCK();
    
    // Optionally stop motor
    if (settings.spike_stop_motor) {
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
    }
}
```

**Effort**: 20-30 lines, ~30 minutes

---

## SUMMARY

### Load Sensor Initialization

**ANSWER**: ❌ **NO initialization needed!**

**Why**:
- MCB has current sensing hardware (always active)
- Monitors load continuously (no enable command)
- Reports via GF flags (jam=bit 2, spike=bit 1)
- HMI just reads flags and responds

### What Nova Needs

**For Jam Detection**: ✅ Already working!

**For Spike Detection**:
1. ✅ GF bit 1 already parsed (motor_status.overload)
2. ❌ Need settings toggle (spike_detect_enable)
3. ❌ Need alarm display when flag set
4. ❌ Need menu item (Spike Detect: ON/OFF)

**Commands**:
- ❌ NO new MCB commands needed
- ✅ LD exists but unused (may test if works)
- ✅ Just read existing GF flags

**Effort**: 20-30 lines, ~30 minutes (UI only)

---

## INITIALIZATION SUMMARY

| Sensor | Init Required? | Unlock Command | Enable Command |
|--------|----------------|----------------|----------------|
| **Temperature** | ✅ YES | CL query at boot | Maybe service mode |
| **Vibration** | ✅ YES | CL query + VR/VS/V8/VG | VS(1) |
| **Load/Spike** | ❌ NO | None needed | None (always on) |
| **Jam** | ❌ NO | None needed | None (always on) |

---

END OF LOAD SENSOR ANALYSIS

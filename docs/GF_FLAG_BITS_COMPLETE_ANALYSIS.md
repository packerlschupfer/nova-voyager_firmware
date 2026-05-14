# GF Flag Bits - Complete Analysis

**Status:** IN PROGRESS - Comprehensive bit-by-bit analysis  
**Date:** January 17, 2026

---

## GF Command Overview

**GF (Get Flags)** - 0x4746 ("GF")
- Query command (no parameters)
- Returns: ASCII decimal number (e.g., "34", "16929")
- Contains motor status, faults, and error flags as bit field

---

## Confirmed Bit Mapping

Based on original firmware analysis (firmware_r2p06k_cg.asm) and hardware testing:

| Bit | Mask | Name | Purpose | Tested? |
|-----|------|------|---------|---------|
| 0 | 0x0001 | FAULT | General motor fault condition | ✅ 2× |
| 1 | 0x0002 | OVERLOAD | Load spike or sustained overload | ✅ 4× |
| 2 | 0x0004 | JAM | Motor stall detected | ✅ 8× |
| 3 | 0x0008 | RPS_ERR_0 | Speed regulation error (low bit) | ❓ |
| 4 | 0x0010 | RPS_ERR_1 | Speed regulation error (high bit) | ❓ |
| 5 | 0x0020 | PFC_FAULT | Power factor correction fault | ✅ Seen |
| 6 | 0x0040 | VOLT_ERR_0 | Voltage out of range (low bit) | ✅ Code |
| 7 | 0x0080 | VOLT_ERR_1 | Voltage out of range (high bit) | ✅ Code |
| 8 | 0x0100 | EXTENDED_0 | Extended voltage/thermal (low) | ❓ |
| 9 | 0x0200 | EXTENDED_1 | Extended voltage/thermal (high) | ✅ 2× |
| 10 | 0x0400 | UNKNOWN_10 | Unknown | ❌ |
| 11 | 0x0800 | UNKNOWN_11 | Unknown | ❌ |
| 12 | 0x1000 | UNKNOWN_12 | Unknown | ❌ |
| 13 | 0x2000 | UNKNOWN_13 | Unknown | ❌ |
| 14 | 0x4000 | MOTOR_STATUS | Motor ready/idle status? | ✅ 2× |
| 15 | 0x8000 | UNKNOWN_15 | Unknown | ❌ |

**Legend:**
- ✅ N× : Actively tested by original firmware (N occurrences)
- ✅ Seen: Observed in hardware testing
- ✅ Code: Referenced in our code
- ❓ : Documented but not confirmed
- ❌ : Not tested by original firmware

---

## Observed Flag Values

### flags=34 (Normal Operation)
```
Binary: 0000 0000 0010 0010
Bits: 1, 5
- Bit 1: OVERLOAD (normal load?)
- Bit 5: PFC_FAULT (minor PFC issue?)
```
**Motor operates normally with these flags.**

### flags=16929 (Power Loss)
```
Binary: 0100 0010 0010 0001
Bits: 0, 5, 9, 14
- Bit 0: FAULT - General fault
- Bit 5: PFC_FAULT - Power quality
- Bit 9: EXTENDED_1 - Severe voltage
- Bit 14: MOTOR_STATUS - Ready/idle?
```
**Motor blocked - multiple voltage faults during power-off event.**

---

## Error String Mapping

Found in firmware strings:
- "LOW Voltage" - Likely bit 6 or 7
- "PFC Fault" - Bit 5 confirmed
- "Invert. OverHeat" - Likely bits 8-9 (thermal)
- "Under Volt Stop" - Severe undervoltage
- "Under Volt Run" - Minor undervoltage

---

## Bits 8-9: Thermal vs Voltage Confusion

**Initial assumption:** Bits 8-9 = Overheat (thermal)  
**Reality:** Bits 8-9 = Extended voltage/thermal range

**Evidence:**
- Set during power-off (voltage event, not thermal)
- flags=16929 has bit 9 set with multiple voltage flags
- May indicate EITHER thermal OR severe voltage

**Hypothesis:** Bits 8-9 are multipurpose:
- Value 0x100 (bit 8): Thermal warning?
- Value 0x200 (bit 9): Voltage critical?
- Value 0x300 (both): Combined thermal + voltage fault?

---

## Bit 14 (0x4000): Motor Status

**Original firmware tests bit 14:**
- At 0x80045f8: `and.w r0, r0, #16384` (0x4000)
- If set: Returns immediately (exits function)
- If clear: Continues processing

**Hypothesis:** Bit 14 = Motor Ready/Idle status
- Set when motor is idle and ready
- Clear when motor is busy/initializing

**Observed:** Set during power-off event (motor definitely idle!)

---

## Original Firmware Bit Tests

From disassembly analysis (`analyze_gf_flags.py`):

```
Bit 0:  tested 2 times  - General fault
Bit 1:  tested 4 times  - Overload/spike (frequent check)
Bit 2:  tested 8 times  - Jam detection (most frequent!)
Bit 9:  tested 2 times  - Extended voltage/thermal
Bit 14: tested 2 times  - Motor status/ready
```

**Bits 3-8, 10-13, 15:** Not actively tested by original firmware
- May be reserved
- May be read-only status
- May be unused

---

## MCB Autonomous Protection

**The MCB handles protection autonomously:**
1. Monitors voltage, current, temperature continuously
2. Sets appropriate GF flag bits when faults detected
3. May reduce power or shut down motor automatically
4. HMI reads GF flags for display/logging only

**HMI should NOT try to "fix" faults** - just monitor and display.

---

## Recommended GF Flag Handling

```c
// Read GF flags
uint16_t flags = motor_query_flags();

// Check critical faults (motor won't run)
if (flags & 0x0001) {
    // Bit 0: General fault - check other bits for details
}

if (flags & 0x0004) {
    // Bit 2: Jam detected - motor stalled
    display_error("Drill Bit Jam");
    motor_stop();
}

if (flags & 0x0300) {
    // Bits 8-9: Extended voltage/thermal fault
    // MCB should handle autonomously
    log_warning("Voltage/thermal fault");
}

if (flags & 0x00C0) {
    // Bits 6-7: Voltage error
    display_warning("Low Voltage");
}

// Informational flags (motor may still run)
if (flags & 0x0002) {
    // Bit 1: Overload - high load condition
    update_load_indicator();
}

if (flags & 0x0020) {
    // Bit 5: PFC fault - power quality issue
    log_warning("PFC Fault");
}
```

---

## TODO: Complete Analysis

**Still needed:**
1. Find what triggers bits 10-13
2. Confirm bit 14 meaning (motor status vs fault)
3. Test thermal overheat (bits 8-9) with actual heating
4. Correlate all error strings with specific bits
5. Test each flag condition if possible

**Method:**
- Search for each error string address in disassembly
- Trace back to what GF bit triggers it
- Test conditions (jam, overheat, voltage drop, etc.)
- Document complete 16-bit mapping

---

END OF GF FLAG ANALYSIS

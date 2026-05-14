# GF Flag Bits - Quick Reference

**Last Updated:** January 17, 2026

---

## 16-Bit Flag Field

```
Bit:  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └─ FAULT (General)
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └─── OVERLOAD
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └───── JAM
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └─────── RPS_ERR (low)
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └───────── RPS_ERR (high)
      ││ ││ ││ ││ ││ ││ ││ ││ ││ ││ └─────────── PFC_FAULT
      ││ ││ ││ ││ ││ ││ ││ ││ ││ └───────────── VOLT_ERR (low)
      ││ ││ ││ ││ ││ ││ ││ ││ └─────────────── VOLT_ERR (high)
      ││ ││ ││ ││ ││ ││ ││ └───────────────── THERMAL/VOLT_EXT (low)
      ││ ││ ││ ││ ││ ││ └─────────────────── THERMAL/VOLT_EXT (high)
      ││ ││ ││ ││ ││ └───────────────────── UNKNOWN_10
      ││ ││ ││ ││ └─────────────────────── UNKNOWN_11
      ││ ││ ││ └───────────────────────── UNKNOWN_12
      ││ ││ └─────────────────────────── UNKNOWN_13
      ││ └───────────────────────────── MOTOR_STATUS?
      └─────────────────────────────── UNKNOWN_15
```

---

## Confirmed Bits

| Bit | Mask | Name | When Set | Action |
|-----|------|------|----------|--------|
| **0** | 0x0001 | **FAULT** | General fault condition | Stop motor |
| **1** | 0x0002 | **OVERLOAD** | High load/spike detected | Monitor |
| **2** | 0x0004 | **JAM** | Motor stalled | Stop motor, display "Drill Bit Jam" |
| **5** | 0x0020 | **PFC_FAULT** | Power factor correction issue | Warning |
| **6-7** | 0x00C0 | **VOLT_ERR** | Voltage out of range | Warning/stop |
| **9** | 0x0200 | **EXTENDED** | Severe voltage or thermal | Stop motor |
| **14** | 0x4000 | **MOTOR_STATUS** | Motor ready/idle (hypothesis) | Status only |

---

## Common Flag Patterns

### Normal Operation
```
flags=34 (0x0022)
- Bit 1: OVERLOAD (may be normal)
- Bit 5: PFC_FAULT (minor)
Motor works fine
```

### Power Loss
```
flags=16929 (0x4221)
- Bit 0: FAULT
- Bit 5: PFC_FAULT
- Bit 9: EXTENDED voltage
- Bit 14: MOTOR_STATUS
Motor blocked
```

---

## Error Strings Found

| String | Likely Bit(s) | Flash Address |
|--------|---------------|---------------|
| "LOW Voltage" | 6-7 or 8-9 | 0x8007248 |
| "PFC Fault" | 5 ✅ | 0x800726c |
| "Invert. OverHeat" | 8-9 (thermal) | 0x8007278 |
| "Under Volt Stop" | 9? | 0x8016b94 |
| "Under Volt Run" | 6-7? | 0x8016ba4 |
| "Drill Bit Jam" | 2 ✅ | 0x800cac8 |

---

## Usage in Nova Firmware

```c
uint16_t flags = motor_get_flags();

// Critical faults (block motor)
if (flags & 0x0001) {
    // General fault
}

if (flags & 0x0004) {
    // Jam detected
    display_error("Drill Bit Jam");
    motor_stop();
}

if (flags & 0x0300) {
    // Extended voltage/thermal
    log_warning("SEVERE VOLTAGE FLAG");
}

// Warnings (motor may run)
if (flags & 0x00C0) {
    // Voltage error (bits 6-7)
    display_warning("Low Voltage");
}

if (flags & 0x0002) {
    // Overload
    update_load_display();
}
```

---

## Further Investigation Needed

**To complete the mapping:**
1. Trace each error string to GF bit test
2. Test undervoltage condition (trigger bits 6-7)
3. Test thermal overheat (trigger bits 8-9 thermal)
4. Identify bits 10-13, 15 purpose
5. Confirm bit 14 (motor status vs fault)

---

END OF QUICK REFERENCE

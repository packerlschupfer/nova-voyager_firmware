# MCB Commands with Dual Purpose

**Commands that behave differently as QUERY vs COMMAND**

---

## CL - Current Limit

### CL as QUERY
```
Format: [SOH][0][0][1][1][1]['C']['L'][ENQ]
Response: "XX" (current limit setting: 20, 50, or 70)
Purpose: Read current limit configuration
```

### CL(0) as COMMAND  
```
Format: [SOH][0][0][1][1][STX][1]['C']['L']['0'][ETX][XOR]
Response: ACK
Purpose: Clear/reset motor controller state
```

**Discovery:** Original firmware uses BOTH forms:
- CL query at boot (0x800afa4) - reads configuration
- CL(0) command in sensor init (0x801a588) - clears state

**In our firmware:**
- CL query: Breaks motor operation (leaves MCB in wrong state)
- CL(0) command: Not tested, may work differently

---

## T0 - Temperature Baseline

### T0 as QUERY
```
Format: [SOH][0][0][1][1][1]['T']['0'][ENQ]
Response: "TXXX" (e.g., "T018" = 18°C)
Purpose: Read current MCB heatsink temperature
```

### T0(0) as COMMAND
```
Format: [SOH][0][0][1][1][STX][1]['T']['0']['0'][ETX][XOR]
Response: ACK
Purpose: Set thermal baseline to 0 (initialization)
```

**Discovery:** Original firmware uses BOTH forms:
- T0 query for reading temperature (menu display)
- T0(0) command for sensor init (during first boot?)

**In our firmware:**
- T0 query: Works! Shows MCB heatsink temp (17-20°C)
- T0(0) command: Breaks motor operation

---

## Pattern Recognition

**Several MCB commands follow this dual-purpose pattern:**

| Command | As QUERY | As COMMAND |
|---------|----------|------------|
| **CL** | Read current limit | Clear motor state |
| **T0** | Read MCB temperature | Set thermal baseline |
| **TH** | Read threshold config | Set threshold value |
| **HT** | Read threshold (not temp!) | N/A |

**Query format:** Ends with [ENQ] (0x05), no parameters
**Command format:** [STX]...[parameters]...[ETX][checksum]

---

## Usage Recommendations

**For nova_firmware:**

1. **Avoid during boot:**
   - CL query: Breaks motor ❌
   - CL(0) command: Not tested
   - T0(0) command: Breaks motor ❌

2. **Safe during runtime:**
   - T0 query: Works! ✅ (via TEMPMCB command or menu)
   - Other queries: Test carefully

3. **Original firmware behavior:**
   - Uses both QUERY and COMMAND forms
   - Timing and context matter
   - May only use COMMAND on first boot (factory setup)

---

END OF DUAL PURPOSE COMMANDS

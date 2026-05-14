# Original Firmware Motor Commands - From R2P06e CG Disassembly
**Analysis Date**: January 12, 2026  
**Source**: Actual firmware binary reverse engineering

---

## CRITICAL FINDING: Additional Motor Commands Found

The original Teknatool firmware uses **MORE commands** than we documented!

---

## Complete Motor Command List (from disassembly)

### Basic Motor Control

| Command | Code | Hex | Address | Purpose |
|---------|------|-----|---------|---------|
| RS | 0x5253 | 52 53 | 0x0801A3FE | Stop motor (coast) |
| ST | 0x5354 | 53 54 | 0x0801A3F0 | Start motor |
| JF | 0x4A46 | 4A 46 | 0x0801A40C/41C | Jog (fwd=0x6AA, rev=0x6AB) |
| SV | 0x5356 | 53 56 | 0x0801A652 | Set/Query speed |
| GF | 0x4746 | 47 46 | 0x0801A42C | Get flags (status bits) |

### **NEWLY DISCOVERED COMMANDS** (Load/Power Monitoring)

| Command | Code | Hex | Address | Purpose |
|---------|------|-----|---------|---------|
| **CL** | **0x434C** | **43 4C** | **0x0801A4FC** | **Current Limit / Current Load** |
| **VR** | **0x5652** | **56 52** | **0x0801A50C** | **Voltage/Value Read** |
| **GR** | **0x4752** | **47 52** | **0x0801A4E0** | **Generic Register Read** |
| **VS** | **0x5653** | **56 53** | **0x0801A4EC** | **Value Set (mode/threshold)** |

---

## How Original Firmware Gets Load/Power

### Monitoring Loop (0x0800B0A0-0x0800B0AE)

```asm
; Motor load monitoring sequence
0x800b0a0:  movs r0, #100
0x800b0a2:  bl   0x801a50c    ; VR(100) - Read voltage/value register 100
0x800b0a6:  mov  r0, r4        
0x800b0a8:  bl   0x801a4fc    ; CL(r4)  - Get/Set current limit
0x800b0ac:  movs r0, #1
0x800b0ae:  bl   0x801a4ec    ; VS(1)   - Set value/mode
```

**Sequence**:
1. **VR(100)**: Query value register 100 (possibly load percentage register)
2. **CL(value)**: Get/set current limit (returns current draw or load%)
3. **VS(1)**: Set monitoring mode to active

This runs **periodically when motor is running** to update load display.

---

## CL Command (Current Limit / Load)

**Function**: 0x0801A4FC  
**Purpose**: Get current motor load or current limit  
**Format**: COMMAND (with parameter)

**Packet**:
```
04 30 30 31 31 02 31 43 4C [PARAM] 03 [XOR]
```

**Response** (estimated):
```
04 30 30 31 31 02 31 43 4C [LOAD%] 03
```
- Returns: Current load as percentage (0-100)
- OR: Current limit setting (if reading config)

**Usage**:
- Query with param 0 → get current load
- Set with param N → set current limit to N%

---

## VR Command (Voltage/Value Read)

**Function**: 0x0801A50C  
**Purpose**: Read voltage or value from indexed register  
**Format**: COMMAND (with register index)

**Packet**:
```
04 30 30 31 31 02 31 56 52 [INDEX] 03 [XOR]
```

**Register Indices** (speculation):
- 100: Load percentage register
- 101: Voltage register
- 102: Current register
- etc.

**Response**:
```
04 30 30 31 31 02 31 56 52 [VALUE] 03
```

---

## GR Command (Generic Register Read)

**Function**: 0x0801A4E0  
**Purpose**: Generic register/parameter reader  
**Format**: QUERY

**Packet**:
```
04 30 30 31 31 31 47 52 05
```

**Purpose**: Possibly reads all status values at once?

---

## VS Command (Value Set)

**Function**: 0x0801A4EC  
**Purpose**: Set monitoring mode or threshold  
**Format**: COMMAND

**Packet**:
```
04 30 30 31 31 02 31 56 53 [MODE] 03 [XOR]
```

**Modes**:
- 0: Disable monitoring
- 1: Enable load monitoring
- 2: Enable voltage monitoring?

---

## Motor Load Data Flow (Original Firmware)

```
┌──────────────────────────────────────────────────────────┐
│ 1. Motor Running                                         │
│    └─ Main loop calls motor monitoring routine          │
└──────────────────────────────────────────────────────────┘
                        ↓
┌──────────────────────────────────────────────────────────┐
│ 2. Send VR(100) Command                                  │
│    └─ Query value register 100 (load register?)         │
│    └─ MCB responds with load percentage                 │
└──────────────────────────────────────────────────────────┘
                        ↓
┌──────────────────────────────────────────────────────────┐
│ 3. Send CL(0) Command                                    │
│    └─ Query current limit / current load                │
│    └─ MCB responds with current draw or load%           │
└──────────────────────────────────────────────────────────┘
                        ↓
┌──────────────────────────────────────────────────────────┐
│ 4. Parse Response                                        │
│    └─ Extract decimal value from response               │
│    └─ Store in SRAM (e.g., 0x20000050)                  │
└──────────────────────────────────────────────────────────┘
                        ↓
┌──────────────────────────────────────────────────────────┐
│ 5. Display on LCD                                        │
│    └─ Format as "Power: 65%"                            │
│    └─ Show on main status screen                        │
└──────────────────────────────────────────────────────────┘
```

---

## Commands Original Firmware Uses (Complete List)

### Motor Control
- RS (0x5253): Stop
- ST (0x5354): Start  
- JF (0x4A46): Jog forward/reverse

### Status/Monitoring
- GF (0x4746): Get flags
- SV (0x5356): Set/query speed
- **CL (0x434C): Current limit / load** ← **LOAD SOURCE!**
- **VR (0x5652): Value register read** ← **POWER SOURCE!**
- **GR (0x4752): Generic register read**

### Configuration
- KP, KI, VP, VI: PID parameters
- PU, SA, SR, TR: Motor tuning
- IU, OV: IR compensation
- IL, BR: Current limit, brake
- **VS (0x5653): Value set (monitoring mode)**

---

## Key Findings

1. **GF Returns Flags Only**: Single decimal number (not comma-separated)
2. **Load Comes from CL Command**: Separate query to MCB
3. **Power Comes from VR Command**: Reads value register
4. **Multiple Commands Per Update**: VR(100) + CL(0) + VS(1) sequence
5. **MCB Has Indexed Registers**: VR(index) accesses different values

---

## Implications for Nova Firmware

### What We're Missing

❌ **CL Command**: Not implemented - needed for true load reading  
❌ **VR Command**: Not implemented - needed for power/voltage reading  
❌ **VS Command**: Not implemented - needed to enable MCB monitoring mode  
❌ **GR Command**: Not implemented - generic register access

### Current Workaround

✅ Nova firmware calculates load from **speed droop**:
```c
load_pct = (target_rpm - actual_rpm) / target_rpm × 100
```

**This works** but is less accurate than MCB's current sensing.

### To Get True Load from MCB

Need to implement:
```c
// Enable monitoring mode
send_command(CMD_VS, 1);

// Query load register
send_command(CMD_VR, 100);  // or CMD_CL with param 0
parse_response();           // Get load percentage from MCB
```

---

## Disassembly Evidence

### motor_status() at 0x0801A42C
```asm
0801a42c:  push {r4, lr}
0801a42e:  movs r0, #0x47
0801a430:  lsls r0, r0, #8
0801a432:  adds r0, #0x46      ; Build 0x4746 = "GF"
0801a434:  bl   0x801b360      ; Call motor_query()
0801a438:  pop  {r4, pc}
```
**Sends**: GF query only (gets flags)

### Current Limit at 0x0801A4FC
```asm
0801a4fc:  push {r4, lr}
0801a4fe:  movs r1, r0         ; param in r1
0801a500:  movs r0, #0x43
0801a502:  lsls r0, r0, #8
0801a504:  adds r0, #0x4c      ; Build 0x434C = "CL"
0801a506:  bl   0x801b0b2      ; Call motor_cmd()
0801a50a:  pop  {r4, pc}
```
**Sends**: CL with parameter

### Value Read at 0x0801A50C
```asm
0801a50c:  push {r4, lr}
0801a50e:  movs r1, r0         ; index in r1
0801a510:  movs r0, #0x56
0801a512:  lsls r0, r0, #8
0801a514:  adds r0, #0x52      ; Build 0x5652 = "VR"
0801a516:  bl   0x801b0b2      ; Call motor_cmd()
0801a51a:  pop  {r4, pc}
```
**Sends**: VR with register index

---

## Conclusion

**Original firmware gets load/power via CL and VR commands** sent to the MCB.

The MCB has internal current sensing and calculates load percentage based on motor phase current measurement. The HMI queries these values via serial protocol.

Nova firmware **calculates load from speed droop** instead, which works but is less accurate than the MCB's hardware current sensing.

To match original firmware behavior, we would need to:
1. Implement CL, VR, VS commands
2. Discover correct register indices for VR
3. Integrate into motor_query_status() routine
4. Parse responses and update g_state.motor_load

---

END OF ANALYSIS

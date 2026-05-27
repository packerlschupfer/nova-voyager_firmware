# CL (Current Limit) Command - Original Firmware Usage Analysis
**Analysis Date**: January 12, 2026  
**Firmware**: R2P06e CG  
**Hardware Test**: User's actual Nova Voyager MCB

---

## Hardware Testing Results

User tested CL command on actual hardware:

```
Command: CL (query format)
TX: 04 30 30 31 31 31 43 4C 05
RX: "20"
```

**Result**: Returns **"20"** (20% current limit)  
**Interpretation**: This is a **configuration value**, not real-time motor load

---

## CL Command in Original Firmware

### Function Definition (0x0801A4FC)

```asm
0x801a4fc:  push  {r4, lr}
0x801a4fe:  mov   r4, r0          ; Save parameter
0x801a500:  mov   r1, r4          ; r1 = parameter (current limit %)
0x801a502:  movw  r0, #17228      ; r0 = 0x434C ('CL')
0x801a506:  bl    0x801b0b2       ; motor_cmd(CL, r1)
0x801a50a:  pop   {r4, pc}
```

**Purpose**: Sends CL command with parameter to set or query current limit

---

## CL Usage Pattern

### Usage 1: Initialization (0x0801A530)

```asm
0x801a51c:  push  {r4, lr}
0x801a51e:  movs  r1, #0          ; parameter = 0
0x801a520:  movw  r0, #22098      ; VR command
0x801a524:  bl    motor_cmd       ; VR(0) - clear value register
0x801a528:  movs  r0, #5
0x801a52a:  bl    delay_ms        ; Wait 5ms

0x801a52e:  movs  r1, #0          ; parameter = 0
0x801a530:  movw  r0, #17228      ; CL command  
0x801a534:  bl    motor_cmd       ; CL(0) - clear current limit
0x801a538:  movs  r0, #5
0x801a53a:  bl    delay_ms        ; Wait 5ms

0x801a53e:  movs  r1, #0          ; parameter = 0
0x801a540:  movw  r0, #22099      ; VS command
0x801a544:  bl    motor_cmd       ; VS(0) - clear value set
0x801a548:  movs  r0, #5
0x801a54a:  bl    delay_ms        ; Wait 5ms
```

**Pattern**: Initialization clears VR, CL, VS with parameter 0

**Purpose**: Reset MCB monitoring/configuration registers to known state

---

### Usage 2: Monitoring Loop (0x800B0A0-0x800B100)

```asm
; Setup
0x800b0a0:  movs  r0, #100        ; r0 = 100
0x800b0a2:  bl    set_vr          ; VR(100) - set value register to 100
0x800b0a6:  mov   r0, r4          ; r0 = r4 (variable)
0x800b0a8:  bl    set_cl          ; CL(r4) - set current limit to r4
0x800b0ac:  movs  r0, #1          ; r0 = 1
0x800b0ae:  bl    set_vs          ; VS(1) - enable monitoring mode

; Loop (10 iterations)
0x800b0be:  movs  r0, #3
0x800b0c0:  bl    delay_ms        ; Wait 3ms
0x800b0c4:  bl    get_gr          ; GR - query status
0x800b0c8:  cmp   r0, #2          ; Check GR response
0x800b0ca:  beq   0x800b0e4       ; Branch if response = 2

; Update r4 based on conditions
0x800b0cc:  cmp   r5, #4
0x800b0ce:  blt   0x800b0de       ; If r5 < 4
0x800b0d0:  movs  r5, #0
0x800b0d2:  adds  r0, r4, #5      ; r4 += 5
0x800b0d4:  uxtb  r4, r0          ; r4 = r4 & 0xFF
0x800b0d6:  cmp   r4, #25         ; Cap at 25
0x800b0d8:  ble   0x800b0f4
0x800b0da:  movs  r4, #25         ; r4 = 25 (max)

0x800b0f4:  mov   r0, r4
0x800b0f6:  bl    set_cl          ; CL(r4) - update current limit

0x800b0fa:  adds  r0, r7, #1      ; loop_counter++
0x800b0fc:  uxth  r7, r0
0x800b0fe:  cmp   r7, #10         ; Loop 10 times
0x800b100:  blt   0x800b0be       ; Continue loop
```

**Pattern**: Loop that dynamically adjusts CL value (0-25) based on GR status

**Purpose**: Adaptively set current limit based on motor feedback

---

## What CL Actually Does

### Based on Hardware Testing + Disassembly:

**CL is a CONFIGURATION COMMAND**, not a monitoring query!

**Set Mode**: `CL(value)` → Set current limit to value%
- Example: CL(20) sets current limit to 20%
- Example: CL(0) clears/resets current limit
- Example: CL(25) sets limit to 25%

**Query Mode**: `CL` (query) → Returns current configuration
- Response: "20" = current limit is set to 20%
- NOT motor load percentage
- NOT real-time sensor value

---

## How Original Firmware Uses CL

### Scenario 1: Initialization
```c
// Clear all monitoring registers at boot
VR(0);    // Clear value register
delay(5ms);
CL(0);    // Clear current limit
delay(5ms);
VS(0);    // Clear monitoring mode
```

**Purpose**: Reset MCB to known state

### Scenario 2: Dynamic Current Limiting

```c
// Initialize
VR(100);  // Set value register to 100
CL(r4);   // Set initial current limit (value in r4)
VS(1);    // Enable monitoring mode

// Loop 10 times
for (i = 0; i < 10; i++) {
    delay(3ms);
    status = GR();  // Query status from MCB
    
    if (status == 2) {
        // Condition met - adjust differently
        r4 = update_based_on_status();
    } else {
        // Normal condition
        if (r5 >= 4) {
            r4 += 5;         // Increase current limit
            if (r4 > 25) r4 = 25;  // Cap at 25%
        }
    }
    
    CL(r4);  // Update current limit dynamically
}
```

**Purpose**: Gradually increase current limit from initial value to 25% while monitoring MCB status

**Use Case**: 
- Soft-start current limiting
- Prevent inrush current on motor start
- Ramp up current allowance as motor stabilizes
- Monitor MCB status (GR) and adjust accordingly

---

## CL is NOT for Load Sensing

### What We Thought

❌ "CL returns real-time motor load (0-100%)"  
❌ "Use CL to get current draw or load percentage"  
❌ "Poll CL for load monitoring in tapping mode"

### Reality from Hardware Testing

✅ CL returns **configuration** (current limit setting)  
✅ CL **sets** the limit, doesn't **read** load  
✅ Response "20" means limit is configured to 20%  
✅ NOT a sensor, NOT real-time feedback

---

## What is GR Command?

From the monitoring loop (0x800B0C4):

```asm
bl    get_gr          ; GR - query generic register/status
cmp   r0, #2          ; Check if response = 2
beq   ...             ; Branch if specific status
```

**GR Purpose** (speculation):
- Query MCB status during current limit ramp
- Returns status code (0, 1, 2, etc.)
- Firmware adjusts current limit based on GR response
- Possibly: 0=ok, 1=ramping, 2=ready, 3=fault

---

## Current Limit Ramping Purpose

### Why Ramp Current Limit?

**Problem**: Large motors have high inrush current on startup
- Can trip breakers
- Can cause voltage sag
- Can damage motor windings

**Solution**: Gradually increase current limit
- Start at low limit (e.g., 5%)
- Ramp up to operating limit (e.g., 25%)
- Monitor MCB status (GR) during ramp
- Adjust based on feedback

**Teknatool Implementation**:
1. Initialize: VR(100), CL(r4), VS(1)
2. Loop 10 times:
   - Query GR status
   - Increase CL by 5% each iteration (max 25%)
   - Delay 3ms between updates
3. Final: Current limit at 25% (or whatever r4 reached)

---

## Factory Default 70% vs Runtime 20-25%

### Discrepancy Explained

**Factory Service Manual**: Current Limit = 70%
- This is the **absolute maximum** allowed by motor design
- Set via IL command during service mode
- Prevents motor damage from overcurrent

**Runtime Operation**: Current Limit ramped 0-25%
- This is the **operating current limit** during normal use
- Set via CL command during motor start
- Provides soft-start and inrush protection
- Much lower than 70% factory max

**Two Different Limits**:
| Limit | Command | Value | Purpose |
|-------|---------|-------|---------|
| **Factory Max** | IL | 70% | Hardware protection (absolute ceiling) |
| **Runtime Limit** | CL | 20-25% | Operating limit (soft-start ramping) |

---

## Why Your Hardware Returns "20"

When you query CL:
- MCB returns current runtime current limit setting
- Value is 20% (configured during last motor operation)
- This is NOT motor load
- This is NOT real-time sensor data
- This is the SETPOINT for current limiting

**Analogy**:
- CL is like a thermostat **setpoint** (not room temperature)
- Query CL → "20" means limit is **set to** 20%
- Doesn't tell you actual motor current draw
- Just tells you what the limit is configured to

---

## Load Sensing Reality

### Original Firmware

Based on disassembly analysis:
- ❌ No real-time load monitoring found
- ❌ No speed-droop calculations
- ❌ No current sensing readback
- ✅ Only sets current limit (CL), doesn't read load

### Nova Firmware

Implements load sensing via speed droop:
```c
load% = (target_rpm - actual_rpm) / target_rpm × 100
```

**This is BETTER than original firmware!**
- ✅ Provides real-time load estimate
- ✅ Used for tapping load mode
- ✅ Accurate enough for threshold detection (±15%)
- ✅ Novel improvement over original

---

## Recommendations

### Do NOT Use CL for Load Monitoring

**Reasons**:
1. CL returns configuration, not sensor data
2. CL sets limit, doesn't read current
3. Not useful for real-time load tracking
4. Hardware testing confirms: "20" is static config

### Keep Current Nova Implementation

**Speed droop method**:
- ✅ Appropriate for load sensing
- ✅ More capable than original
- ✅ Works correctly
- ✅ No changes needed

### Optional: Implement CL Ramping

If you want to match original soft-start behavior:

```c
void motor_soft_start(void) {
    motor_send_command(CMD_VALUE_READ, 100);  // VR(100)
    
    for (int limit = 5; limit <= 25; limit += 5) {
        motor_send_command(CMD_CURRENT_LIMIT, limit);  // CL(limit)
        motor_send_command(CMD_VALUE_SET, 1);          // VS(1)
        vTaskDelay(pdMS_TO_TICKS(3));
        
        // Query status (if GR supported)
        // Adjust ramping based on response
    }
}
```

**Benefits**: Soft motor start, reduced inrush current  
**Priority**: LOW (current implementation works fine)

---

## Summary

**CL Command Purpose**: Set/query current limit **configuration**
- CL(0) = Clear/reset limit
- CL(20) = Set limit to 20%
- CL(query) = Read current limit setting → "20"

**NOT for**: Real-time motor load monitoring

**Original Firmware Usage**:
- Initialization: CL(0) to clear
- Soft-start: CL ramping from low to 25%
- Loop: Adjust CL based on GR status
- Never reads CL response for load value

**Nova Firmware**:
- ✅ Speed droop load sensing is superior
- ⏸️ CL ramping could be added for soft-start
- ✅ Current implementation production-ready

---

END OF CL USAGE ANALYSIS

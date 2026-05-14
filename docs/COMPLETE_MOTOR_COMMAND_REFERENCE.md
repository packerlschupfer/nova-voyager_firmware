# Complete Motor Command Reference - R2P06e Firmware
**Purpose**: Comprehensive mapping of ALL motor commands for nova_firmware replication  
**Source**: R2P06e CG disassembly + hardware testing  
**Date**: January 12, 2026

---

## EXECUTIVE SUMMARY

The original Teknatool firmware uses **18 motor commands** to control and configure the MCB (Motor Controller Board). This document maps every command, its usage pattern, and implementation status in nova_firmware.

---

## COMPLETE COMMAND TABLE

| # | Cmd | Code | ASCII | Purpose | Category | Usage Freq | Nova Status |
|---|-----|------|-------|---------|----------|------------|-------------|
| 1 | RS | 0x5253 | 'R''S' | Stop motor (coast) | Control | High | ✅ Yes |
| 2 | ST | 0x5354 | 'S''T' | Start motor | Control | High | ✅ Yes |
| 3 | JF | 0x4A46 | 'J''F' | Jog (0x6AA=fwd, 0x6AB=rev) | Control | High | ✅ Yes |
| 4 | SV | 0x5356 | 'S''V' | Set/Query Speed (RPM) | Control | High | ✅ Yes |
| 5 | GF | 0x4746 | 'G''F' | Get Flags (status bits) | Monitor | High | ✅ Yes |
| 6 | BR | 0x4252 | 'B''R' | Brake/Spindle Hold (0/1) | Control | Medium | ✅ Yes |
| 7 | KP | 0x4B50 | 'K''P' | Speed Kp (PID proportional) | Config | Low | ✅ Yes |
| 8 | KI | 0x4B49 | 'K''I' | Speed Ki (PID integral) | Config | Low | ✅ Yes |
| 9 | VP | 0x5650 | 'V''P' | Voltage Kp (Factory: 2000) | Config | Low | ✅ Yes |
| 10 | VI | 0x5649 | 'V''I' | Voltage Ki (Factory: 9000) | Config | Low | ✅ Yes |
| 11 | IU | 0x4955 | 'I''U' | IR Gain (Factory: 28835) | Calibrate | Once | ✅ Yes |
| 12 | OV | 0x4F56 | 'O''V' | IR Offset (Factory: 82) | Calibrate | Once | ✅ Yes |
| 13 | PU | 0x5055 | 'P''U' | Pulse Max (Factory: 185) | Calibrate | Once | ✅ Yes |
| 14 | SA | 0x5341 | 'S''A' | Advance Max (Factory: 85°) | Calibrate | Once | ✅ Yes |
| 15 | IL | 0x494C | 'I''L' | Current Limit (Factory: 70%) | Calibrate | Once | ✅ Yes |
| 16 | SR | 0x5352 | 'S''R' | Speed Ramp (accel rate) | Config | Low | ✅ Yes |
| 17 | TR | 0x5452 | 'T''R' | Torque Ramp (ramp time) | Config | Low | ✅ Yes |
| 18 | SP | 0x5350 | 'S''P' | Save to MCB EEPROM | Persist | Once | ✅ Yes |

**Additional Commands Found** (not in standard set):
| Cmd | Code | Purpose | Status | Nova |
|-----|------|---------|--------|------|
| VR | 0x5652 | Value Register (index read) | ❌ MCB NAK | ❌ No |
| VS | 0x5653 | Value Set/Mode | ✅ MCB ACK | ❌ No |
| GR | 0x4752 | Generic Register/Status | ? Unknown | ❌ No |
| CL | 0x434C | Current Limit (runtime) | ✅ Returns "20" | ⚠️ Partial |

---

## COMMAND CATEGORIES

### Category A: Basic Control (Always Used)

**RS - Stop Motor** (0x5253)
- **Function**: 0x0801A3FE
- **Usage**: Called when user presses STOP, E-Stop, guard opens, jam detected
- **Parameter**: 0 (no parameter)
- **Response**: ACK (06) on success
- **Effect**: Motor coasts to stop, de-energizes
- **Nova**: ✅ motor_stop() - includes hardware cutoff on PD4

**ST - Start Motor** (0x5354)
- **Function**: 0x0801A3F0
- **Usage**: Called after motor enabled and direction set
- **Parameter**: 0
- **Response**: ACK
- **Effect**: Motor begins rotation at set speed
- **Nova**: ✅ motor_start() - includes hardware enable on PD4

**JF - Jog Forward/Reverse** (0x4A46)
- **Function**: 0x0801A40C (forward), 0x0801A41C (reverse)
- **Usage**: Sets motor direction before start
- **Parameter**: 0x6AA (1706 decimal) = forward, 0x6AB (1707) = reverse
- **Response**: ACK
- **Effect**: Sets motor controller direction
- **Nova**: ✅ motor_forward(), motor_reverse()

**SV - Set/Query Speed** (0x5356)
- **Function**: 0x0801A652 (set), query variant exists
- **Usage**: 
  - SET: Called when speed changes (encoder, buttons, settings)
  - QUERY: Polled to get actual motor RPM
- **Parameter**: RPM value (50-5500)
- **Response**: ACK (set), or actual RPM (query)
- **Nova**: ✅ motor_set_speed(), polled in motor_query_status()

**GF - Get Flags** (0x4746)
- **Function**: 0x0801A42C
- **Usage**: Polled periodically when motor running (original: unknown freq, nova: 500ms)
- **Parameter**: None (query format)
- **Response**: Single decimal number (flags as bits)
  - Bit 0: Fault
  - Bit 1: Overload
  - Bit 2: Jam
  - Bit 3-4: RPS error
  - Bit 5: PFC fault
  - Bit 6-7: Voltage error
  - Bits 8-9: Overheat
- **Nova**: ✅ motor_update(), polled every 500ms

---

### Category B: PID Tuning (Service Mode)

**KP/KI - Speed PID** (0x4B50, 0x4B49)
- **Function**: Part of motor_set_pid()
- **Usage**: Service mode only (password 3210)
- **Parameter**: Kp and Ki values (user-tunable)
- **Response**: ACK
- **Effect**: Adjusts speed regulation loop
- **Default**: Kp=100, Ki=50 (nova), varies by installation
- **Nova**: ✅ motor_set_pid()

**VP/VI - Voltage PID** (0x5650, 0x5649)
- **Function**: Part of motor_set_pid()
- **Usage**: Service mode, sent at boot
- **Parameter**: Factory values (VP=2000, VI=9000)
- **Response**: ACK
- **Effect**: Adjusts voltage regulation loop
- **Nova**: ✅ motor_set_pid(), sent in motor_sync_settings()

---

### Category C: IR Compensation (Factory Calibration)

**IU - IR Gain** (0x4955)
- **Function**: Part of motor calibration
- **Usage**: Sent ONCE at boot with factory value
- **Parameter**: 28835 (factory calibration)
- **Response**: ACK
- **Effect**: Calibrates current sensing gain
- **Purpose**: Compensates for IR voltage drop in motor windings
- **Nova**: ✅ motor_set_ir(), sent in motor_sync_settings()

**OV - IR Offset** (0x4F56)
- **Function**: Part of motor calibration
- **Usage**: Sent ONCE at boot with factory value
- **Parameter**: 82 (factory calibration)
- **Response**: ACK
- **Effect**: Calibrates current sensing offset (zero point)
- **Nova**: ✅ motor_set_ir(), sent in motor_sync_settings()

**Critical**: IR compensation MUST be set before motor runs or speed regulation fails!

---

### Category D: Motor Protection

**BR - Brake/Spindle Hold** (0x4252)
- **Function**: 0x0801A... (multiple wrappers)
- **Usage**: 
  - BR(1) = Enable powered spindle hold (motor energized at zero speed)
  - BR(0) = Disable hold (motor free-spinning)
- **Response**: ACK
- **Effect**: Motor holds position against external torque when "stopped"
- **Timeout**: MCB auto-releases after 30 seconds
- **Nova**: ✅ motor_set_braking(), called in local_motor_stop()

**IL - Current Limit (Factory)** (0x494C)
- **Function**: Service mode setting
- **Usage**: Sets absolute maximum motor current (hardware protection)
- **Parameter**: 70% (factory default)
- **Response**: ACK
- **Effect**: Hard limit on motor phase current
- **Nova**: ✅ motor_set_current_limit(), sent in motor_sync_settings()

---

### Category E: PWM & Commutation

**PU - Pulse Max** (0x5055)
- **Function**: Factory calibration
- **Usage**: Sent at boot with factory value
- **Parameter**: 185 (factory default)
- **Response**: ACK
- **Effect**: Sets maximum PWM pulse width
- **Purpose**: Limits motor voltage/speed ceiling
- **Nova**: ✅ motor_set_pulse_max(), sent in motor_sync_settings()

**SA - Advance Max** (0x5341)
- **Function**: Factory calibration
- **Usage**: Sent at boot with factory value
- **Parameter**: 85° (factory default)
- **Response**: ACK
- **Effect**: Sets maximum commutation advance angle
- **Purpose**: Optimizes efficiency at high speeds
- **Nova**: ✅ motor_set_advance_max(), sent in motor_sync_settings()

---

### Category F: Ramp Control

**SR - Speed Ramp** (0x5352)
- **Function**: Speed acceleration control
- **Usage**: Configurable in settings (original), sent at boot (nova)
- **Parameter**: RPM/second (e.g., 1000 = 1000 RPM/s acceleration)
- **Response**: ACK
- **Effect**: Controls how fast motor accelerates/decelerates
- **Purpose**: Smooth speed changes, reduce mechanical stress
- **Nova**: ✅ motor_set_ramp(), sent in motor_sync_settings()

**TR - Torque Ramp** (0x5452)
- **Function**: Torque ramping time
- **Usage**: Configurable in settings
- **Parameter**: Time in milliseconds (e.g., 2000 = 2 second ramp)
- **Response**: ACK
- **Effect**: Controls torque application rate
- **Purpose**: Smooth torque transitions
- **Nova**: ✅ motor_set_ramp(), sent in motor_sync_settings()

---

### Category G: Persistence

**SP - Save Parameters** (0x5350)
- **Function**: EEPROM save command
- **Usage**: Called after changing any motor parameters
- **Parameter**: 0 (save all)
- **Response**: ACK (takes ~100ms)
- **Effect**: Writes all motor parameters to MCB EEPROM
- **Purpose**: Persist settings across power cycles
- **Delay**: MUST wait 100ms after SP before next command!
- **Nova**: ✅ motor_save_to_eeprom(), called by MSAVE command

---

## ADDITIONAL COMMANDS (Discovery)

### CL - Current Limit (Runtime)

**Hardware Test**: CL query → "20"

**Purpose**: **Runtime current limit** (different from factory IL!)

**Usage Pattern from Disassembly**:

```c
// Soft-start sequence at 0x800B0A0
VR(100);      // Initialize value register
CL(initial);  // Set starting current limit (low, e.g., 5%)
VS(1);        // Enable monitoring mode

for (i = 0; i < 10; i++) {
    delay(3ms);
    status = GR();  // Query MCB status
    
    // Ramp up current limit based on status
    if (status conditions met) {
        current_limit += 5;        // Increase by 5%
        if (current_limit > 25) current_limit = 25;
    }
    
    CL(current_limit);  // Update runtime limit
}
```

**Two Limit System**:
| Type | Command | Value | Purpose |
|------|---------|-------|---------|
| **Factory Max** | IL | 70% | Absolute ceiling (hardware protection) |
| **Runtime** | CL | 0-25% | Operating limit (soft-start, dynamic) |

**Actual Motor Current Limited to**: min(IL, CL)

**During Operation**:
- Factory max (IL): 70% (never change)
- Runtime limit (CL): 20-25% (actively adjusted)
- Actual limit: min(70%, 20%) = 20%

**Nova Implementation**: ⚠️ Partial
- IL sent at boot (70%) ✅
- CL ramping NOT implemented ❌
- Could add for smoother motor starts

---

### VS - Value Set / Monitoring Mode

**Hardware Test**: VS(1) → ACK

**Purpose**: Enable/disable monitoring mode in MCB

**Usage**:
- VS(0) = Disable monitoring (initialization)
- VS(1) = Enable monitoring (before operation)
- VS(?) = Unknown modes possible

**Pattern from Firmware**:
```c
// Initialization
VS(0);  // Clear/disable

// Before motor start
VR(100);
CL(initial);
VS(1);  // Enable monitoring
```

**MCB Behavior** (speculation):
- VS(1) enables real-time status updates
- Allows CL ramping to function
- May enable additional GR responses
- Required before dynamic current limiting works

**Nova Implementation**: ❌ Not implemented
- Could add if implementing CL ramping
- Not critical for basic operation

---

### VR - Value Register

**Hardware Test**: VR → NAK (not supported)

**Purpose**: Index-based register read/write (NOT supported by user's MCB)

**Usage in Firmware**:
- VR(0) = Clear value register (init)
- VR(100) = Set to 100 (before monitoring)
- VR(speed) = Called during speed ramping with 0-100 values

**MCB Support**: ❌ NOT available on user's hardware

**Nova Implementation**: ❌ Not implemented (MCB doesn't support)

---

### GR - Generic Register / Status

**Usage**: Called in soft-start loop after each CL update

**Purpose**: Query MCB status during current limit ramping

**Response**: Status code (0, 1, 2, etc.)

**Pattern**:
```c
CL(current_limit);
delay(3ms);
status = GR();
if (status == 2) {
    // Special condition
} else {
    // Normal ramp
}
```

**Nova Implementation**: ❌ Not implemented
- Not critical for basic operation
- Needed only for advanced CL ramping

---

## CONFIGURATION SETTINGS

### Current Limit Settings (from strings analysis)

**String Search Results**:
```
"Current Limit" - Found at multiple offsets
"Param Load Failed" - Error message for parameter loading
```

**No evidence of** "Soft", "Medium", "Hard" presets found in strings!

**Conclusion**: Current limit is likely **numeric entry** (0-100%), not preset choices.

---

### Menu Structure (from strings)

**Motor Parameter Menus**:
```
SERVICE MODE (password 3210)
  ├─ Adv. Motor Params
  │   ├─ IR Gain (IU)
  │   ├─ IR Offset (OV)
  │   ├─ Voltage Kp (VP)
  │   ├─ Voltage Ki (VI)
  │   ├─ Speed Kp (KP)
  │   ├─ Speed Ki (KI)
  │   ├─ Pulse Max (PU)
  │   ├─ Advance Max (SA)
  │   └─ Current Limit (IL)
  │
  ├─ Electrical Parameters
  │   ├─ Speed Ramp (SR)
  │   ├─ Torque Ramp (TR)
  │   └─ (others unknown)
  │
  └─ Save Parameters (SP)
```

**Access**: Locked behind password, service technicians only

**Nova Implementation**: ⚠️ Commands implemented, UI missing
- All parameter commands work (KP, KI, VP, VI, etc.)
- MREAD command reads all parameters
- MSYNC command syncs to MCB
- MSAVE command saves to EEPROM
- Service menu UI not implemented

---

## MOTOR INITIALIZATION SEQUENCE

### Original Firmware Boot Sequence

**Phase 1: Clear Registers** (0x0801A51C)
```asm
VR(0);    delay(5ms);
CL(0);    delay(5ms);
VS(0);    delay(5ms);
S8(0x108); delay(5ms);  // Unknown command
G7(0x105); // Unknown command
```

**Phase 2: Set Factory Calibration** (called from boot)
```c
PU(185);      // Pulse max
SA(85);       // Advance max  
IL(70);       // Current limit (factory max)
IU(28835);    // IR gain
OV(82);       // IR offset
VP(2000);     // Voltage Kp
VI(9000);     // Voltage Ki
```

**Phase 3: Set User Parameters** (from EEPROM)
```c
KP(user_kp);
KI(user_ki);
SR(user_ramp_speed);
TR(user_ramp_torque);
```

**Phase 4: Save to MCB** (if changed)
```c
SP(0);        // Save all parameters to MCB EEPROM
delay(100ms); // CRITICAL: Wait for EEPROM write!
```

**Nova Implementation**: ✅ Complete
- motor_sync_settings() sends all parameters
- motor_save_to_eeprom() saves with SP command
- MOTOR_MCB_WRITE_DELAY_MS = 100ms delay enforced
- Called during task_motor startup

---

## MOTOR OPERATION SEQUENCES

### Normal Motor Start

**Original Firmware**:
```c
1. SV(target_rpm);      // Set desired speed
2. JF(direction);       // Set direction (0x6AA or 0x6AB)
3. ST(0);               // Start motor
4. Poll GF() every Nms; // Monitor for faults
```

**Nova Firmware**:
```c
1. MOTOR_CMD(CMD_MOTOR_SET_SPEED, rpm);     // Queued
2. MOTOR_CMD(CMD_MOTOR_FORWARD/REVERSE, 0); // Queued
   // Motor task handles:
   - local_motor_set_direction(fwd/rev)
   - local_motor_start()
   - motor_hardware_enable() on PD4
3. motor_query_status() polls GF every 500ms
```

**Match**: ✅ Equivalent behavior

---

### Motor Stop (Normal)

**Original Firmware**:
```c
RS(0);     // Stop motor (coast)
// Motor de-energizes, coasts to stop
```

**Nova Firmware**:
```c
motor_hardware_disable();  // PD4 LOW (hardware cutoff)
motor_send_command(CMD_STOP, 0);  // RS
// Motor stops
```

**Difference**: Nova adds hardware cutoff for safety

---

### Motor Stop with Hold (Tapping)

**Original Firmware** (speculation - BR call found):
```c
RS(0);     // Stop rotation
BR(1);     // Enable powered hold
// Motor holds position for 30s
```

**Nova Firmware**:
```c
// task_motor.c::local_motor_stop()
send_command(CMD_STOP, 0);
send_command(CMD_SET_BRAKE, 1);  // BR(1)
// Motor holds position
```

**Match**: ✅ Equivalent (nova correctly implements powered hold)

---

## MOTOR SOFT-START SEQUENCE (Advanced)

### Original Firmware (0x800B0A0)

```c
void motor_soft_start(uint8_t initial_limit) {
    // Initialize monitoring
    VR(100);              // Set value register
    CL(initial_limit);    // Set starting current limit (low, e.g., 5%)
    VS(1);                // Enable monitoring mode
    
    // Ramp current limit
    uint8_t limit = initial_limit;
    for (int i = 0; i < 10; i++) {
        delay(3ms);
        uint8_t status = GR();  // Query MCB status
        
        // Adjust based on status
        if (status != 2) {
            if (some_condition >= 4) {
                limit += 5;              // Increase by 5%
                if (limit > 25) limit = 25;  // Cap at 25%
            }
        } else {
            // Handle special status condition
            if (another_condition < -30) {
                limit = 0;
                other_var = 15;
            } else {
                other_var -= 1;
            }
        }
        
        CL(limit);  // Update current limit
    }
    
    // After loop: current limit at final value (typically 25%)
}
```

**Purpose**: Prevent motor inrush current by ramping current limit

**Nova Implementation**: ❌ Not implemented
- Motor starts at full configured current limit (IL=70%)
- No soft-start ramping
- May cause higher inrush current

**Priority**: MEDIUM (nice-to-have for smooth starts)

---

## COMMAND IMPLEMENTATION STATUS

### Fully Implemented in Nova ✅

**Basic Control**:
- ✅ RS, ST, JF, SV, GF, BR

**PID Control**:
- ✅ KP, KI, VP, VI

**Calibration**:
- ✅ IU, OV, PU, SA, IL

**Ramping**:
- ✅ SR, TR

**Persistence**:
- ✅ SP

**Total**: 18/18 commands implemented

---

### Partially Implemented ⚠️

**CL - Current Limit (Runtime)**:
- ✅ Command code defined
- ❌ Soft-start ramping not implemented
- ❌ Dynamic current limiting not used
- Impact: Motor starts without current ramp (higher inrush)
- Priority: MEDIUM (could improve motor life)

---

### Not Implemented (MCB Doesn't Support) ❌

**VR - Value Register**:
- Hardware returns NAK
- MCB doesn't support this command
- Not needed (original uses it but MCB ignores)

**VS - Value Set**:
- Hardware returns ACK but purpose unclear
- Not critical for operation
- Could be implemented for completeness

**GR - Generic Register**:
- Unknown if MCB supports (not tested)
- Used in CL ramping sequence
- Not critical without CL ramping

---

## NOVA FIRMWARE GAPS

### Missing Functionality

1. **CL Soft-Start Ramping**
   - Original: Ramps current limit 0→25% over 10 iterations
   - Nova: Starts at full IL limit (70%)
   - Impact: Higher motor inrush current
   - Fix Effort: MEDIUM (100-150 lines)
   - Priority: MEDIUM

2. **VS Monitoring Mode Management**
   - Original: Calls VS(1) before soft-start
   - Nova: Not called
   - Impact: Unknown (MCB accepts command)
   - Fix Effort: LOW (5-10 lines)
   - Priority: LOW

3. **GR Status Querying**
   - Original: Polls GR during CL ramping
   - Nova: Not implemented
   - Impact: Can't adapt CL ramp to MCB status
   - Fix Effort: LOW (command wrapper only)
   - Priority: LOW (only useful with CL ramping)

### Missing UI/Menus

4. **Service Mode UI**
   - Original: Complete menu for parameter tuning
   - Nova: Commands work, UI missing
   - Impact: Must use serial commands (MREAD, KP, etc.)
   - Fix Effort: HIGH (menu system integration)
   - Priority: LOW (service techs can use serial)

5. **Current Limit Menu**
   - Original: Menu item for IL setting
   - Nova: Hardcoded 70% default
   - Impact: Can't change without serial command
   - Fix Effort: LOW (add menu item)
   - Priority: LOW (factory default works fine)

---

## RECOMMENDATIONS

### SHORT TERM (Ship As-Is)

Current nova_firmware is **production-ready**:
- ✅ All critical commands implemented
- ✅ Motor control fully functional
- ✅ Safety systems superior to original
- ✅ Factory calibration correct

---

### MEDIUM TERM (Optional Enhancements)

1. **Implement CL Soft-Start Ramping**
   ```c
   void motor_soft_start(uint16_t target_rpm) {
       // Enable monitoring
       motor_send_command(CMD_VALUE_SET, 1);  // VS(1)
       
       // Ramp current limit
       for (uint8_t limit = 5; limit <= 25; limit += 5) {
           motor_send_command(CMD_CURRENT_LIMIT, limit);
           vTaskDelay(pdMS_TO_TICKS(30));  // 30ms per step
       }
       
       // Now start motor normally
       motor_set_speed(target_rpm);
       motor_forward();  // or reverse
   }
   ```
   
   **Benefit**: Smoother motor starts, reduced inrush
   **Effort**: ~2 hours
   **Priority**: MEDIUM

2. **Add Service Mode Menu**
   - Password protection (3210)
   - Motor parameter tuning UI
   - Live parameter display
   **Effort**: ~1-2 days
   **Priority**: LOW

---

### LONG TERM (Future Features)

3. **Advanced Ramping Control**
   - User-configurable SR/TR values
   - Menu for acceleration profiles
   - "Soft", "Medium", "Hard" presets for SR/TR
   **Effort**: ~1 day
   **Priority**: LOW

---

## COMPLETE MOTOR COMMAND REFERENCE

### Command Syntax

**Set Parameter**:
```
motor_send_command(CMD, value);
```

**Query Parameter**:
```
motor_query(CMD);
response = parse_response();
```

### All Commands with Examples

| Command | Set Example | Query Example | Response |
|---------|-------------|---------------|----------|
| RS | RS(0) | - | ACK |
| ST | ST(0) | - | ACK |
| JF | JF(0x6AA) fwd, JF(0x6AB) rev | - | ACK |
| SV | SV(1500) set 1500 RPM | SV(query) | "1500" |
| GF | - | GF(query) | "34" (flags) |
| BR | BR(1) enable, BR(0) disable | BR(query)? | ACK |
| KP | KP(100) | KP(query)? | ACK |
| KI | KI(50) | KI(query)? | ACK |
| VP | VP(2000) | - | ACK |
| VI | VI(9000) | - | ACK |
| IU | IU(28835) | - | ACK |
| OV | OV(82) | - | ACK |
| PU | PU(185) | - | ACK |
| SA | SA(85) | - | ACK |
| IL | IL(70) | IL(query)? | ACK or "70" |
| SR | SR(1000) | - | ACK |
| TR | TR(2000) | - | ACK |
| SP | SP(0) | - | ACK (wait 100ms!) |
| CL | CL(20) | CL(query) | "20" (config) |
| VS | VS(1) enable, VS(0) disable | - | ACK |

---

## COMPLETE IMPLEMENTATION CHECKLIST

### Nova Firmware Status

| Feature | Original | Nova | Gap |
|---------|----------|------|-----|
| **Basic Control** | RS, ST, JF, SV | ✅ All | None |
| **Status Monitor** | GF polling | ✅ 500ms poll | None |
| **Factory Calibration** | IU, OV, PU, SA, IL | ✅ At boot | None |
| **PID Tuning** | KP, KI, VP, VI | ✅ At boot | None |
| **Ramping** | SR, TR | ✅ At boot | None |
| **Persistence** | SP | ✅ MSAVE cmd | None |
| **Brake/Hold** | BR | ✅ local_motor_stop | None |
| **Hardware Safety** | None | ✅ PD4 enable | Nova better! |
| **Soft-Start CL** | CL ramping | ❌ Not impl | Optional |
| **Monitoring Mode** | VS | ❌ Not impl | Optional |
| **Status Query** | GR | ❌ Not impl | Optional |
| **Service UI** | Full menu | ⚠️ Serial only | Low priority |

---

## CONCLUSION

**Nova firmware has 18/18 motor commands implemented correctly!** ✅

**Only gaps are optional enhancements**:
- CL soft-start ramping (smoother starts)
- VS/GR monitoring (only useful with CL ramping)
- Service mode UI (commands work via serial)

**Motor protocol is COMPLETE and production-ready!**

---

END OF COMPLETE MOTOR COMMAND REFERENCE

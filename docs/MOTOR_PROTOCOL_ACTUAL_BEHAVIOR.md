# Motor Protocol - Actual Behavior vs Documentation
**Discovery Date**: January 12, 2026  
**Source**: Hardware testing feedback

---

## CRITICAL DISCOVERY: GF Response Format

### What We THOUGHT GF Returns:
```
GF Response: "0,750,65,120,45"
              ↑  ↑   ↑   ↑   ↑
              flags,rpm,load,vib,temp
```

### What GF ACTUALLY Returns:
```
GF Response: "34"  (just flags, single decimal number!)
              ↑
              flags only
```

---

## Actual Motor Protocol Commands

### GF (Get Flags) - 0x4746

**Format**: QUERY  
**Sent**: `04 30 30 31 31 31 47 46 05`  
**Response**: `04 30 30 31 31 02 31 47 46 [FLAGS] 03` (no checksum)  
**Data**: Single ASCII decimal number representing flag bits

**Example**:
- Response: "34" (decimal)
- Binary: 0b00100010
- Flags:
  - Bit 0: Fault (0)
  - Bit 1: Overload (1) ✓
  - Bit 2: Jam (0)
  - Bit 3-4: RPS error (0)
  - Bit 5: PFC fault (1) ✓
  - Bit 6-7: Voltage error (0)

**Returns**: Flags ONLY (not speed, load, vib, temp)

---

### SV (Set/Query Speed) - 0x5356

**Format**: QUERY (for reading)  
**Sent**: `04 30 30 31 31 31 53 56 05`  
**Response**: `04 30 30 31 31 02 31 53 56 [SPEED] 03`  
**Data**: ASCII decimal number (actual RPM)

**Example**:
- Response: "750" 
- Meaning: Motor running at 750 RPM

**Returns**: Actual motor speed (RPM)

---

### Load Calculation - NOT FROM MCB!

**CRITICAL**: Load is NOT read from MCB - it's CALCULATED by HMI!

**Method**: Speed droop under load (task_motor.c:647-659)

```c
// Calculate load from speed regulation error
if (actual_speed < target_speed) {
    uint16_t error = target_speed - actual_speed;
    uint16_t load_pct = (error * 100) / target_speed;
    g_state.motor_load = load_pct;
}
```

**Example**:
- Target: 1000 RPM
- Actual: 750 RPM (from SV query)
- Error: 250 RPM
- Load: (250 / 1000) × 100 = 25%

**Rationale**:
- Motor slows down under load (speed droop)
- Speed error proportional to load torque
- No dedicated current sensor needed
- Works for any motor controller

---

## Status Query Sequence

The HMI polls motor status every 500ms with TWO commands:

### Query 1: GF (Get Flags)
```
Send: 04 30 30 31 31 31 47 46 05
Recv: 04 30 30 31 31 02 31 47 46 "34" 03

Parse: flags = 34
       g_state.motor_fault = (flags & 0x80)
```

### Query 2: SV (Get Speed)  
```
Send: 04 30 30 31 31 31 53 56 05
Recv: 04 30 30 31 31 02 31 53 56 "750" 03

Parse: actual_speed = 750
       g_state.current_rpm = 750
       
Calculate: load = (target - actual) / target × 100
           g_state.motor_load = load
```

**Total**: Two serial transactions per status update

---

## Commands That DON'T Exist (or aren't used)

These were speculation based on incomplete RE:

❌ **UP** (Update Power) - No evidence  
❌ **LD** (Load Direct) - Not implemented  
❌ **GT** (Get Temperature) - Not used  
❌ **GV** (Get Vibration) - Not used  
❌ **GP** (Get Power) - Not used  

**Temperature** field in code (line 496) is present but never updates (temp always 0)

---

## Actual MCB Capabilities

Based on hardware testing, the MCB firmware is **simpler** than assumed:

**Supported**:
- ✅ GF: Get flags (fault status)
- ✅ SV: Set/query speed  
- ✅ RS: Stop motor
- ✅ JF: Jog forward/reverse
- ✅ ST: Start motor
- ✅ PID parameter commands (KP, KI, VP, VI, etc.)

**NOT Supported** (or not exposed):
- ❌ Direct load readout
- ❌ Direct current measurement
- ❌ Vibration sensor data
- ❌ Temperature sensor (or not wired)
- ❌ Comma-separated multi-field responses

---

## Implications for Nova Firmware

### What Works ✅

1. **Flag Monitoring**: GF returns fault flags correctly
2. **Speed Monitoring**: SV returns actual RPM correctly
3. **Load Calculation**: Speed error method is valid for drill press
4. **Tapping Load Mode**: Works with calculated load (not direct measurement)

### What Doesn't Work ❌

1. **Direct Current Sensing**: Not available from MCB
2. **Temperature Monitoring**: MCB doesn't report temp (always 0)
3. **Vibration Sensing**: Not implemented in MCB firmware
4. **Multi-Field GF**: Documentation was incorrect

### What to Fix 🔧

**motor.c::parse_gf_response()** (line 261):
- Currently tries to parse comma-separated fields
- Should just parse single decimal number as flags
- Fields 1-4 (speed, load, vib, temp) won't populate
- This is OK - speed comes from SV, load is calculated

**Recommendation**: Update parse_gf_response() to:
```c
static void parse_gf_response(size_t len) {
    // GF returns single ASCII decimal number (flags only)
    // Find data after command echo
    size_t data_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (rx_buffer[i] == PROTO_STX) {
            data_start = i + 4;  // Skip STX, unit, cmd_H, cmd_L
            break;
        }
    }
    
    if (data_start == 0) return;
    
    // Parse flags as single decimal number
    int16_t flags = parse_decimal(rx_buffer, data_start, len - data_start - 1);
    
    motor_status.raw_flags = (uint16_t)flags;
    motor_status.fault = (flags & 0x01) != 0;
    motor_status.overload = (flags & 0x02) != 0;
    motor_status.jam_detected = (flags & 0x04) != 0;
    motor_status.rps_error = (flags & 0x18) != 0;
    motor_status.pfc_fault = (flags & 0x20) != 0;
    motor_status.voltage_error = (flags & 0xC0) != 0;
    motor_status.overheat = (flags & 0x300) != 0;
    
    // Load, speed, temp NOT in GF response
    // Speed comes from SV query
    // Load calculated from speed error
    // Temp not available from MCB
}
```

---

## Load Sensing Accuracy

**Method**: Speed droop calculation  
**Formula**: Load% = (Target - Actual) / Target × 100

**Accuracy**: ±10-15% (estimate)
- Good enough for tapping load threshold detection
- Not as accurate as direct current sensing
- Affected by motor characteristics and PID tuning

**Sufficient For**:
- ✅ Detecting when tap engages material (load jumps from 0% to 20%+)
- ✅ Triggering reverse on high load (threshold typically 60%)
- ✅ Through-hole detection (load drops to <5%)
- ✅ Chip-breaking peck cycling

**Not Suitable For**:
- ❌ Precise torque measurement
- ❌ Motor current limiting
- ❌ Power consumption monitoring

---

## Testing Commands

Add these to nova_firmware for MCB protocol exploration:

### TESTGF - Test GF with different formats

```c
void cmd_testgf(void) {
    uart_puts("Testing GF command formats:\r\n");
    
    // Test 1: Query format (current)
    uart_puts("\n1. QUERY format:\r\n");
    motor_test_qq('G', 'F');
    
    // Test 2: Command format with param 0
    uart_puts("\n2. COMMAND format (param 0):\r\n");
    motor_test_mq('G', 'F', 0);
    
    // Test 3: Command format with param 1
    uart_puts("\n3. COMMAND format (param 1):\r\n");
    motor_test_mq('G', 'F', 1);
    
    // Test 4: Command format with param 255
    uart_puts("\n4. COMMAND format (param 255):\r\n");
    motor_test_mq('G', 'F', 255);
}
```

### TESTSV - Test SV query

```c
void cmd_testsv(void) {
    uart_puts("Testing SV query:\r\n");
    motor_test_qq('S', 'V');
}
```

### TESTUNK - Test unknown commands

```c
void cmd_testunk(void) {
    uart_puts("Testing unknown commands:\r\n");
    
    uart_puts("\nUP (Update/Power?):\r\n");
    motor_test_qq('U', 'P');
    
    uart_puts("\nLD (Load?):\r\n");
    motor_test_qq('L', 'D');
    
    uart_puts("\nGP (Get Power?):\r\n");
    motor_test_qq('G', 'P');
    
    uart_puts("\nGT (Get Temp?):\r\n");
    motor_test_qq('G', 'T');
    
    uart_puts("\nGV (Get Vibration?):\r\n");
    motor_test_qq('G', 'V');
    
    uart_puts("\nGL (Get Load?):\r\n");
    motor_test_qq('G', 'L');
}
```

---

## Conclusion

**GF Command**: Returns flags only (not comma-separated)  
**Load Source**: Calculated from speed droop (target - actual)  
**Temperature**: Not available from MCB  
**Vibration**: Not implemented  

**Nova firmware code is CORRECT** - it already calculates load from speed error!

The documentation in parse_gf_response() comments was based on **speculation** from reverse engineering. Actual hardware testing reveals the **simpler reality**.

---

## Recommendations

1. **Update Comments**: Fix parse_gf_response() documentation
2. **Simplify Parser**: Remove comma-parsing logic (not needed)
3. **Document Load Calc**: Clarify load comes from speed droop
4. **Test Unknown Commands**: Probe MCB for hidden features
5. **Accept Limitations**: MCB firmware is simpler than assumed

The current implementation **works correctly** despite incorrect documentation!

---

END OF ACTUAL BEHAVIOR ANALYSIS

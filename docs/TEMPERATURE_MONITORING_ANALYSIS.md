# Temperature Monitoring - R2P06e Firmware Analysis
**Discovery**: Real-time temperature querying via HT command  
**Date**: January 12, 2026

---

## ANSWER TO YOUR QUESTIONS

### Q: Does temperature update live in the menu?
**A: YES** - Temperature is queried **in real-time**, not cached!

### Q: What happens when motor is cold?
**A: Should show ~20-25°C** (room temperature from sensor)

### Q: Is it a real sensor?
**A: YES** - HT (0x4854) command queries MCB thermistor

---

## TEMPERATURE MONITORING FLOW

```
Menu Display Update (periodic)
    ↓
Temperature Menu Item Handler (0x8015C76)
    ↓
HT Query Function (0x801A4A0)
    ├─ Load HT command (0x4854)
    ├─ Send via motor_query (0x801B360)
    ├─ Disable interrupts during query
    ├─ Wait for response
    └─ Re-enable interrupts
    ↓
Response (temperature in °C)
    ↓
Save to r5 register
    ↓
Display "T HtSink = {value}°C"
    ↓
Temperature Control Logic (0x8006EBA)
    ├─ Compare with 99°C threshold
    ├─ If > 99°C:
    │   ├─ Check "Invert OverHeat" setting
    │   ├─ Display "Let motor cool down"
    │   └─ Maybe stop motor (setting-dependent)
    └─ If ≤ 99°C: Continue normal operation
```

---

## TEMPERATURE COMMAND

**HT (0x4854)** - Heat/Thermal Query

**Function**: 0x801A4A0  
**Protocol**: Motor query (QUERY format)  
**Packet**: `04 30 30 31 31 31 48 54 05`  
**Response**: Temperature value in °C (ASCII decimal)  
**Example**: "45" = 45°C

---

## OVERHEAT THRESHOLD

**Critical Temperature**: **99°C**

From code at 0x8006F0C:
```asm
cmp r4, #99      ; Compare temperature with 99
ble n, 0x6f38    ; If <= 99, normal operation
```

**Response Options** (setting-dependent):
1. Display warning "Invert. OverHeat"
2. Stop motor automatically  
3. Current de-rating (reduce power)

**Manual Says**: 60°C for current reduction  
**Code Shows**: 99°C for shutdown/warning

**Possible**: Two thresholds
- 60°C: Start current de-rating
- 99°C: Critical overheat shutdown

---

## UPDATE CHARACTERISTICS

**Query Frequency**: Every menu display update  
**Timing**: ~1-10Hz (depends on menu refresh rate)  
**Method**: Active query (not passive monitoring)  
**Cached**: NO - fresh query each cycle  
**Live**: YES - updates continuously while menu visible  

---

## NOVA FIRMWARE IMPLICATIONS

### What We Have

```c
// task_motor.c lines 496-530
current_temp = temp;  // Store for monitoring

if (temp >= TEMP_SHUTDOWN_DEFAULT) {
    // Critical overheat - immediate shutdown
    if (motor_enabled) {
        uart_puts("OVERHEAT SHUTDOWN!\r\n");
        local_motor_stop();
        SEND_EVENT(EVT_OVERHEAT);
    }
}
```

**Status**: ✅ Overheat logic exists!

### What's Missing

❌ **HT command not implemented** - Temperature always 0  
❌ **No query to MCB** - Field exists but never updates  
❌ **Display always shows 0°C** - No live temperature  

### How to Fix

```c
// Add to motor_query_status() in task_motor.c

// Query temperature via HT command
send_query(CMD_HT);  // 0x4854
if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
    // Parse temperature response
    int16_t temp = parse_field(rx_buffer, data_start, rx_index - data_start - 1);
    if (temp > 0 && temp <= 150) {
        current_temp = temp;
        
        STATE_LOCK();
        g_state.motor_temperature = temp;  // Add to shared state
        STATE_UNLOCK();
    }
}
```

---

## TESTING RECOMMENDATIONS

If you can test original firmware:

**Test 1: Live Update**
- Enter Service Mode → Adv. Motor Params
- Watch "T HtSink = XX°C" for 30 seconds
- Expected: Value should update (±1-2°C fluctuations)

**Test 2: Cold Start**
- Leave overnight, power on cold
- Check T HtSink immediately
- Expected: ~20-25°C (room temperature)

**Test 3: After Running**
- Run motor 1000 RPM for 2 minutes
- Stop, check T HtSink
- Expected: 35-50°C (elevated from heat)

---

## COMMAND SPECIFICATION

**HT (Heat/Thermal) - 0x4854**

**Format**: QUERY (no parameter)
```
TX: 04 30 30 31 31 31 48 54 05
        ↑           ↑  ↑
      header        H  T
```

**Response**: ASCII decimal temperature in °C
```
RX: 04 30 30 31 31 48 54 [TEMP] 03
                      ↑    ↑
                     echo  value
Example: "45" = 45°C
```

**To Test**:
Add to nova_firmware TESTCL-style command:
```c
void cmd_testht(void) {
    uart_puts("Testing HT (temperature) command:\r\n");
    uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1', 'H', 'T', 0x05};
    
    uart_puts("TX: ");
    for (int i = 0; i < 9; i++) { print_hex_byte(pkt[i]); uart_putc(' '); }
    uart_puts("\r\n");
    
    for (int i = 0; i < 9; i++) motor_putc(pkt[i]);
    
    for (volatile int d = 0; d < 500000; d++);
    uint8_t resp[32];
    int rlen = motor_read_resp(resp, sizeof(resp));
    
    uart_puts("RX: ");
    if (rlen > 0) {
        for (int i = 0; i < rlen; i++) { print_hex_byte(resp[i]); uart_putc(' '); }
        uart_puts("\r\nASCII: ");
        for (int i = 0; i < rlen; i++) {
            if (resp[i] >= '0' && resp[i] <= '9') uart_putc(resp[i]);
        }
        uart_puts("°C\r\n");
    } else {
        uart_puts("(timeout)\r\n");
    }
}
```

---

## CONCLUSION

Temperature monitoring in original firmware is:
- ✅ **Real-time** (queried each menu update)
- ✅ **Sensor-based** (thermistor on MCB heatsink)
- ✅ **Active control** (99°C threshold triggers response)
- ✅ **User-configurable** (overheat response options)

**To implement in nova_firmware**:
1. Add HT command (0x4854) definition
2. Query in motor_query_status()
3. Parse response (ASCII decimal °C)
4. Update g_state.motor_temperature
5. Use existing overheat logic (already implemented!)

---

END OF TEMPERATURE ANALYSIS

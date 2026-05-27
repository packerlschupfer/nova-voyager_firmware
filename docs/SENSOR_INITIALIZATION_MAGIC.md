# Sensor Initialization Magic - The Missing Sequence
**Discovery**: Sensors require CL query + specific initialization order
**Date**: January 12, 2026

---

## CRITICAL DISCOVERY

**The "Magic" is a THREE-PART system**:

1. **CL query at boot** (unlocks sensor subsystem)
2. **Vibration init sequence** (VR→CL→VS→V8→VG)
3. **Service Mode entry** (for HT/LT temperature/load queries)

---

## PART 1: Boot Prerequisite - CL Query

### The Unlock Command (0x80047C6)

**CRITICAL**: Before ANY sensor operations, firmware queries CL:

```c
// At MCU motor init (very early in boot)
send_query(CMD_CURRENT_LIMIT);  // 0x434C - UNLOCK MAGIC
wait_response();
// Response: "20", "50", or "70" (current power output setting)
delay(10ms);
```

**Purpose**: Queries current limit, but **side effect** is unlocking MCB sensor subsystem

**Without This**:
- VR → NAK (vibration report fails)
- HT → NAK/timeout (temperature fails)
- LD → Unknown (load threshold fails)

**With This**:
- VS → ACK (vibration enable works) ✓
- VG → ACK (vibration gain works)
- Sensor subsystem ready

---

## PART 2: Vibration Initialization (0x801A51C)

### Complete Sequence (5 commands, 25ms total)

```c
void init_vibration_sensors(void) {
    // 1. Reset vibration report
    motor_cmd(CMD_VR, 0);           // 0x5652, param 0
    vTaskDelay(pdMS_TO_TICKS(5));

    // 2. Query current limit (confirm unlock)
    send_query(CMD_CURRENT_LIMIT);  // 0x434C
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 3. Vibration sensor status (disable initially)
    motor_cmd(CMD_VS, 0);           // 0x5653, param 0
    vTaskDelay(pdMS_TO_TICKS(5));

    // 4. Vibration threshold 1
    motor_cmd(CMD_V8, 264);         // 0x5638, param 0x108
    vTaskDelay(pdMS_TO_TICKS(5));

    // 5. Vibration gain/sensitivity
    motor_cmd(CMD_VG, 261);         // 0x5647, param 0x105 (HIGH)
    vTaskDelay(pdMS_TO_TICKS(5));
}
```

**After This**:
- VS(1) will enable vibration monitoring
- VG can adjust sensitivity (if MCB supports)
- VR may work (if MCB supports - user's returns NAK)

---

## PART 3: Service Mode for Temperature

### HT Query Restriction (0x8015C76)

**Service Mode Required**: YES (from code analysis)

**Evidence**:
- HT only called from Service Mode menu (0x8015C76)
- Menu at Advanced Motor Params (password-protected)
- No HT queries during normal operation

**Service Mode Entry**:
- Password: 3210 (0xC8A)
- Buttons: ZERO + F2 held simultaneously
- After unlock: Advanced menus accessible

**Possible Service Mode Enable Command**:
- Unknown if MCB has service mode flag
- May be HMI-side restriction only (menu access)
- MCB might respond to HT anytime after CL init

---

## NOVA FIRMWARE - What's Missing

### Current State

```c
// task_motor.c - motor_task_init()
void motor_task_init(void) {
    uart_init();  // USART3 setup

    // Motor enable pin initialization
    // ...

    // MISSING: CL query unlock!
    // MISSING: Vibration init sequence!
}

// Later in task_motor startup:
motor_sync_settings();  // Sends factory defaults
```

### What To Add

```c
void motor_task_init(void) {
    uart_init();

    // Initialize motor enable pin
    // ...

    // CRITICAL: Add sensor unlock sequence
    motor_init_sensor_subsystem();  // ← NEW FUNCTION
}

void motor_init_sensor_subsystem(void) {
    extern void uart_puts(const char* s);

    // Wait for MCB to be ready
    vTaskDelay(pdMS_TO_TICKS(100));

    uart_puts("Unlocking sensors: CL query...\r\n");

    // Step 1: Query CL to unlock sensors
    send_query(CMD_CURRENT_LIMIT);
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        // Parse CL response
        // Should be "20", "50", or "70"
        uart_puts("CL unlocked\r\n");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Step 2: Initialize vibration
    uart_puts("Vibration init...\r\n");

    // VR(0) - may NAK if not supported
    send_command(CMD_VR, 0);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    // CL query again
    send_query(CMD_CURRENT_LIMIT);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    // VS(0) - disable vibration initially
    send_command(CMD_VS, 0);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    // V8(264) - threshold 1
    send_command(CMD_V8, 264);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    // VG(261) - vibration gain
    send_command(CMD_VG, 261);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(5));

    uart_puts("Sensor init complete\r\n");

    // Now motor_sync_settings() can proceed
}
```

---

## TESTING AFTER FIX

Once sensor init is added:

**Test 1: VS should still work**
```
> TESTCL
VS(1) → ACK  (should still work)
```

**Test 2: Try HT**
```
> TESTHT  (or add to TESTCL)
HT → "45" or timeout
```

**Test 3: Try LD**
```
LD(30) → ACK or NAK
(set spike threshold to 30%)
```

---

## WHY THIS WASN'T OBVIOUS

**Nova firmware currently**:
- Sends factory calibration (IU, OV, VP, VI, etc.) ✓
- But SKIPS sensor init sequence ✗
- CL never queried at boot ✗

**Original firmware**:
- Queries CL VERY early (0x80047C6)
- Then runs vibration init (0x801A51C)
- Then sensors work ✓

**The "magic" was hidden in early boot code**, not in the obvious motor init!

---

## COMMAND DEFINITIONS NEEDED

Add to config.h:

```c
// Sensor monitoring commands
#define CMD_HT              0x4854      // "HT" - Heat/thermal query
#define CMD_TH              0x5448      // "TH" - Thermal high threshold
#define CMD_TL              0x544C      // "TL" - Thermal low threshold
#define CMD_T0              0x5430      // "T0" - Thermal baseline
#define CMD_LD              0x4C44      // "LD" - Load threshold (spike detect)
#define CMD_VG              0x5647      // "VG" - Vibration gain/sensitivity
#define CMD_V8              0x5638      // "V8" - Vibration threshold
#define CMD_VR              0x5652      // "VR" - Vibration report (may not work)
#define CMD_VS              0x5653      // "VS" - Vibration sensor enable
```

---

## SUMMARY

**The Missing Magic**:
1. ✅ **CL query at boot** - Unlocks MCB sensor subsystem
2. ✅ **5-command vibration sequence** - Initializes vibration monitoring
3. ⚠️ **Service mode** - May be required for HT (unknown)

**Why Sensors Fail**:
- ❌ Nova firmware doesn't query CL at boot
- ❌ Sensor subsystem never unlocked
- ❌ HT/VR/LD fail due to missing prerequisite

**How to Fix**:
- ✅ Add motor_init_sensor_subsystem() function
- ✅ Call before motor_sync_settings()
- ✅ Query CL, run vibration init sequence
- ✅ Then sensors should work!

**Effort**: 50-100 lines, 1-2 hours

---

END OF SENSOR INITIALIZATION MAGIC

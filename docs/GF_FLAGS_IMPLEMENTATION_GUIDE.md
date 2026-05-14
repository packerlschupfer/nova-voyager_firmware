# GF Flags - Implementation Guide for Nova Firmware

**Purpose:** Practical guide for handling GF motor status flags  
**Based on:** Complete disassembly analysis + Teknatool manual + testing

---

## Quick Bit Reference

```c
#define GF_FAULT          0x0001  // Bit 0: General motor fault
#define GF_OVERLOAD       0x0002  // Bit 1: High load/spike
#define GF_JAM            0x0004  // Bit 2: Motor stall
#define GF_MOTOR_INIT     0x0008  // Bit 3: MCB initializing
#define GF_RPS_ERROR      0x0010  // Bit 4: Rotor position sensor (reserved?)
#define GF_PFC_FAULT      0x0020  // Bit 5: Power/temperature fault
#define GF_VOLT_ERR_MASK  0x00C0  // Bits 6-7: Voltage level (2-bit field)
#define GF_EXT_FAULT_MASK 0x0300  // Bits 8-9: Extended voltage/thermal
#define GF_MOTOR_IDLE     0x4000  // Bit 14: Motor idle status

// Voltage levels (bits 6-7)
#define VOLT_OK           0x0000  // Normal voltage
#define VOLT_UNDER_RUN    0x0040  // Bit 6: Undervoltage (can run)
#define VOLT_LOW          0x0080  // Bit 7: Low voltage (critical)
#define VOLT_CRITICAL     0x00C0  // Both: Severe fault
```

---

## Critical Fault Checking

**These MUST stop motor immediately:**

```c
void check_critical_faults(uint16_t flags) {
    if (flags & GF_FAULT) {
        // Bit 0: General motor fault
        display_error("Motor Fault");
        motor_emergency_stop();
        log_fault("GF_FAULT", flags);
    }
    
    if (flags & GF_JAM) {
        // Bit 2: Motor stalled/jammed
        display_error("Drill Bit Jam");
        motor_emergency_stop();
        log_fault("JAM_DETECTED", flags);
    }
    
    if (flags & GF_EXT_FAULT_MASK) {
        // Bits 8-9: Severe voltage or thermal fault
        uint8_t ext_level = (flags >> 8) & 0x03;
        if (ext_level == 2 || ext_level == 3) {
            display_error("Severe Fault");
            motor_emergency_stop();
            log_fault("EXT_FAULT", flags);
        }
    }
}
```

---

## Warning Monitoring

**These indicate issues but motor may continue:**

```c
void check_warnings(uint16_t flags) {
    // Voltage monitoring (bits 6-7)
    uint8_t volt_level = (flags >> 6) & 0x03;
    switch (volt_level) {
        case 1:  // VOLT_UNDER_RUN
            display_warning("Undervoltage");
            // Motor can run but with reduced power
            break;
        
        case 2:  // VOLT_LOW  
            display_warning("Low Voltage");
            // Should stop soon
            break;
        
        case 3:  // VOLT_CRITICAL
            display_error("Voltage Critical");
            motor_stop();  // Don't emergency stop, just normal stop
            break;
    }
    
    // PFC fault (bit 5)
    if (flags & GF_PFC_FAULT) {
        display_warning("PFC Fault");
        log_warning("PFC", flags);
        // Can be voltage OR temperature issue
        // Check temperature sensors separately
    }
    
    // Overload monitoring (bit 1)
    if (flags & GF_OVERLOAD) {
        uint8_t load_pct = calculate_load_from_speed_error();
        update_load_display(load_pct);
        // This is normal during drilling - not an error!
    }
}
```

---

## Special Handling

### Bit 3 - Wait for MCB Ready

**Pattern from original firmware (0x801a51a):**

```c
// CRITICAL: Wait for MCB initialization before motor commands
void wait_for_mcb_ready(void) {
    uint32_t timeout = 0;
    
    while (timeout < 100) {  // Max 100 iterations
        uint16_t flags = motor_query_flags();
        
        if (!(flags & GF_MOTOR_INIT)) {
            // Bit 3 clear - MCB ready!
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout++;
    }
    
    // Timeout - MCB never became ready
    log_error("MCB init timeout");
}

// Use before sending motor start commands
wait_for_mcb_ready();
motor_send_command(CMD_START, 0);
```

**This may solve our cold boot motor issues!**

### Bit 14 - Motor Idle Status

**Not a fault - informational only:**

```c
if (flags & GF_MOTOR_IDLE) {
    // Motor is idle/stopped
    // This is NORMAL, not an error
    // Some processing can be skipped
    g_state.motor_running = false;
}
```

---

## Complete Check Function

```c
typedef enum {
    MOTOR_OK = 0,
    MOTOR_WARNING,
    MOTOR_FAULT,
    MOTOR_CRITICAL
} motor_health_t;

motor_health_t check_motor_health(uint16_t flags) {
    // CRITICAL faults (immediate stop)
    if (flags & (GF_FAULT | GF_JAM)) {
        return MOTOR_CRITICAL;
    }
    
    if (flags & GF_EXT_FAULT_MASK) {
        uint8_t ext = (flags >> 8) & 0x03;
        if (ext >= 2) return MOTOR_CRITICAL;
    }
    
    // Severe voltage (stop soon)
    uint8_t volt = (flags >> 6) & 0x03;
    if (volt >= 2) {
        return MOTOR_FAULT;
    }
    
    // Warnings (can continue)
    if (flags & (GF_PFC_FAULT | GF_OVERLOAD)) {
        return MOTOR_WARNING;
    }
    
    // Check initialization
    if (flags & GF_MOTOR_INIT) {
        return MOTOR_WARNING;  // Wait for ready
    }
    
    return MOTOR_OK;
}
```

---

## Boot Sequence Recommendation

**Based on bit 3 (MOTOR_INIT) discovery:**

```c
void motor_boot_init(void) {
    // 1. Wait for MCB power-up
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 2. Wait for bit 3 to clear (MCB ready)
    wait_for_mcb_ready();  // Polls GF until bit 3 clears
    
    // 3. Sync motor parameters
    motor_sync_settings();
    
    // 4. Clear any latched fault flags
    motor_query_flags();  // Reading clears latched flags
    
    // 5. Ready for motor commands
    g_motor_initialized = true;
}
```

**This may be why motor doesn't work without the initial GF query!**

---

## Bit Test Frequency (from 20 GF queries)

**Most frequently tested bits:**
1. Bit 1 (OVERLOAD): 19 queries - Most important for normal operation
2. Bit 2 (JAM): 13 queries - Critical safety feature
3. Bit 14 (MOTOR_IDLE): 8 queries - Status monitoring
4. Bit 9 (EXT_FAULT_H): 2 queries - Severe faults only
5. Bit 0 (FAULT): 2 queries - General fault
6. Bit 3 (MOTOR_INIT): 1 query - Special initialization check

**Never tested: Bits 4-8, 10-13, 15**
- Either reserved for future use
- Or read-only status bits not used by HMI

---

## Key Insights

1. **Bit 3 is critical for cold boot!**
   - Original firmware waits for bit 3 to clear
   - We skip this and motor doesn't work
   - Need to implement wait-for-ready

2. **Bit 14 is status, not fault**
   - Set when motor idle (normal state)
   - Should NOT trigger errors

3. **Flags latch and need reading to clear**
   - GF query clears latched fault flags
   - This is why initial query helps motor work

4. **Bits 6-7 are 2-bit voltage encoding**
   - Not individual flags
   - Encode 4 voltage states

5. **Bits 8-9 are multipurpose**
   - Can indicate voltage OR thermal
   - Manual confirms both uses

---

## TODO: Implementation

1. Add wait_for_mcb_ready() during cold boot
2. Implement proper voltage level decoding (bits 6-7)
3. Distinguish thermal vs voltage in bits 8-9
4. Handle bit 14 as status, not fault
5. Test each condition if possible

---

END OF IMPLEMENTATION GUIDE

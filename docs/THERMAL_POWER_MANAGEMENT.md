# Thermal Power Management - HMI Monitoring

**Discovery:** HMI monitors MCB temperature and may reduce power  
**Evidence:** "Reduce power limit" string in firmware at 0x80072a4

---

## Two Levels of Thermal Protection

### 1. MCB Autonomous (Hardware Level)
**Via GF flag bits 8-9:**
- MCB monitors its own heatsink temperature
- Sets GF flag bits when threshold exceeded
- May reduce power or shut down automatically
- Independent of HMI

### 2. HMI Active Monitoring (Software Level)
**Via T0 query:**
- HMI periodically queries MCB temperature (T0 command)
- Compares to thresholds (possibly 99°C from disassembly)
- Displays warnings ("Reduce power limit")
- **May actively reduce motor power output**

**This is proactive protection** - HMI intervenes before MCB hits critical temp!

---

## Code Evidence

**"Reduce power limit" string at 0x80072a4**
- Referenced at 0x8006ed6 in error/warning display code
- Code at 0x8006f14: `cmp r4, #99` - Possibly comparing temp to 99°C
- If temp > threshold: Display warning and reduce power

**Pattern suggests:**
1. Query T0 temperature periodically
2. Compare to threshold (99°C?)
3. If exceeded: Display warning
4. Reduce power output (lower current limit or PWM?)

---

## Our Firmware - NOT YET IMPLEMENTED

**Current status:**
- ✅ Monitor MCB temperature (TEMP command works)
- ✅ Display HMI temperature (TEMPMCU command)
- ❌ NO thermal power reduction logic
- ❌ Rely entirely on MCB autonomous protection

**Potential risk:**
- MCB may overheat before autonomous protection triggers
- No proactive thermal management
- No user warning before shutdown

---

## Recommended Implementation

```c
// Periodic thermal monitoring (call from status poll)
void check_thermal_power_management(void) {
    uint16_t mcb_temp = get_cached_mcb_temp();  // From last T0 query
    
    if (mcb_temp >= 99) {
        // Critical temperature - reduce power immediately
        uart_puts("THERMAL: Reducing power (MCB temp critical)\r\n");
        
        // Reduce current limit
        motor_send_command(CMD_SET_CURRENT_LIMIT, 50);  // Down from 70/100
        
        // Display warning
        STATE_LOCK();
        g_state.error_line1 = "* Reduce power *";
        g_state.error_line2 = " MCB Hot: XXC   ";
        STATE_UNLOCK();
    }
    else if (mcb_temp >= 80) {
        // Warning temperature
        uart_puts("THERMAL: MCB temperature elevated\r\n");
        display_warning("MCB Warm");
    }
    
    // Restore power when cooled
    if (mcb_temp < 70 && power_was_reduced) {
        motor_send_command(CMD_SET_CURRENT_LIMIT, 100);  // Restore
        power_was_reduced = false;
    }
}
```

---

## Temperature Thresholds (Hypothesis)

Based on code analysis and Teknatool manual:

| Threshold | Action | Evidence |
|-----------|--------|----------|
| < 60°C | Normal operation | TH(60) we send |
| 60-80°C | Warning threshold | Settings configurable |
| 80-99°C | Elevated, monitor closely | Code comparison |
| ≥ 99°C | **Reduce power output** | `cmp r4, #99` at 0x8006f14 |
| > 100°C | MCB autonomous shutdown | MCB EEPROM defaults |

---

## TODO: Implementation

1. **Query T0 periodically** (every 5-10 seconds when motor running)
2. **Compare to thresholds** (60°C warn, 80°C elevated, 99°C critical)
3. **Reduce power at 99°C** (lower current limit 100 → 50)
4. **Display warnings** at each threshold
5. **Restore power** when temperature drops below 70°C

**This would match original firmware thermal management!**

---

## Current Limitations

**Without thermal power management:**
- Motor may run at full power even when MCB is hot
- No proactive protection
- Rely on MCB autonomous shutdown (reactive)
- No user warnings before critical shutdown

**With thermal power management:**
- Proactive power reduction extends motor life
- User warnings give time to reduce load
- Graceful thermal throttling (not abrupt shutdown)
- Matches original firmware behavior

---

## Next Steps

1. Implement periodic T0 query (safe during operation)
2. Add thermal threshold checking
3. Implement power reduction (reduce current limit)
4. Test with actual thermal load
5. Verify MCB temp stays under control

**This is an important safety feature to implement!**

---

END OF THERMAL POWER MANAGEMENT ANALYSIS

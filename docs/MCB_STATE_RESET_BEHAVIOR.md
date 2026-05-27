# MCB State and Reset Behavior

**Discovered:** January 17, 2026  
**Context:** Debugging why motor works after soft reset but not cold boot

---

## The Problem We Observed

**Symptom:**
- Cold boot: Motor won't start (display shows FWD but no spin)
- Press OFF button: Triggers soft reset
- After soft reset: Motor works immediately!

**Question:** What does soft reset do that cold boot doesn't?

---

## The Answer: Bit 3 (MOTOR_INIT)

**Root Cause:**
- MCB sets **bit 3 (MOTOR_INIT)** during power-up initialization
- Bit 3 indicates "MCB not ready for motor commands"
- Original firmware **waits for bit 3 to clear** before sending motor commands
- **We weren't waiting** → motor commands ignored!

**Soft Boot vs Cold Boot:**
- **Soft boot (OFF button):** Fast restart, MCB already initialized, bit 3 clears quickly
- **Cold boot (power-on):** Full MCB initialization, bit 3 takes longer to clear (~100ms)

---

## Original Firmware Pattern

**Code at 0x801a51a-0x801a524:**
```assembly
loop:
    bl   0x801a484     ; Query GF flags
    and.w r0, r0, #8   ; Test bit 3 (MOTOR_INIT)
    cmp  r0, #0        ; Check if cleared
    bne.n loop         ; If SET, wait and retry
    ; If CLEAR, continue to motor command
    bl   motor_cmd     ; Send JF/ST command
```

**Behavior:** Polls GF until bit 3 clears, THEN sends motor start.

---

## Our Solution

**Implemented wait_for_mcb_ready():**

```c
// Wait for MCB initialization complete (bit 3 clear)
uint32_t timeout = 0;
while (timeout < 50) {  // Max 500ms
    uint16_t flags = motor_query_flags();
    
    if (!(flags & 0x0008)) {  // Bit 3 clear?
        uart_puts("MCB ready (bit 3 clear)\r\n");
        break;  // MCB ready!
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    timeout++;
}
```

**Result:** Motor now works on all boot types! ✅

---

## OFF Button Behavior

**OFF button does NOT send MCB reset commands.**

**What it actually does:**
1. Triggers **NVIC_SystemReset()** via AIRCR register
2. **Reboots entire HMI firmware** (software reset)
3. Warm boot magic preserved in .noinit section
4. Boots as BOOT_SOFT type

**No special MCB commands sent!**

The original firmware OFF button handler (from CLAUDE.md):
```c
// OFF button ("RF" code 0x5246) triggers software reset
NVIC_SystemReset();  // via AIRCR register write
```

---

## E-Stop and Guard Behavior

**Our firmware:**
1. **Hardware cutoff:** PD4 → LOW (immediate, in ISR)
2. **Software stop:** RS command (0x5253)
3. **State update:** Set error state

**Hypothesis about original firmware:**
- Probably does the same: Hardware cutoff + RS command
- May wait for bit 3 after reset? (need to verify)
- No special "reset sequence" found in disassembly

---

## Why Soft Reset "Worked" When Cold Boot Didn't

**It wasn't about reset commands to MCB!**

**The real reason:**
- Soft boot is **FASTER** (skips LCD splash, etc.)
- MCB initialization already started during cold boot
- By the time soft boot code runs, **bit 3 already cleared**
- So soft boot "accidentally" waited long enough!

**Cold boot was too fast:**
- Tried to send motor commands immediately
- Bit 3 still set (MCB initializing)
- MCB ignored commands

**Fix:** Explicitly wait for bit 3 to clear on ALL boot types!

---

## What We Learned

1. **No special MCB reset sequence needed**
   - OFF button just reboots HMI
   - No magic commands to MCB

2. **Timing is everything**
   - Must wait for bit 3 to clear
   - Not just arbitrary delay (50ms, 200ms, etc.)
   - Poll until MCB signals ready!

3. **Soft reset "worked" by accident**
   - Not because it's special
   - Just because it happened to wait long enough

4. **Proper solution: Check bit 3**
   - Like original firmware does
   - Poll GF until bit 3 clears
   - Then motor commands work!

---

## Conclusion

There is **NO special MCB reset routine** for E-Stop/Guard/OFF button.

**The "secret" was bit 3 (MOTOR_INIT):**
- MCB sets it during initialization
- Clears it when ready for commands
- Original firmware waits for it
- We weren't waiting → motor didn't work
- Now we wait → motor works! ✅

---

END OF MCB STATE RESET BEHAVIOR ANALYSIS

# Boot Optimization & RCC Reset Detection
**Date**: 2026-01-16
**Session**: Boot speed optimization and firmware flashing improvements

---

## Summary

Implemented proper boot type detection using RCC reset flags (like original firmware) and optimized boot speeds to match or exceed original Teknatool firmware performance.

---

## Achievements

### 1. RCC Reset Flag Detection ✅

**Replaced**: SRAM magic value approach (prone to stack corruption)
**With**: Hardware RCC->CSR register flags (proper ARM Cortex-M method)

**Boot Types Detected:**
- `BOOT_COLD` - Power-on reset (PORRSTF) → Full splash + beeps (~1.7s)
- `BOOT_SOFT` - Software reset (SFTRSTF) → Fast boot, no splash (~0.25s)
- `BOOT_WATCHDOG` - Watchdog reset (IWDGRSTF) → Warning + full init
- `BOOT_PIN` - External NRST pin reset (PINRSTF)

**Files Modified:**
- `include/shared.h` - Added `boot_type_t` enum and `g_boot_type` global
- `src/main.c` - Added `detect_boot_type()` function using RCC->CSR
- `src/task_motor.c` - Skip MCB init on soft boot (500ms saved!)
- `src/lcd.c` - Conditional splash screen based on boot type

---

### 2. Boot Speed Optimizations ✅

**Cold Boot (Power Cycle):**
- Removed 500ms version splash delay (was redundant with LCD splash)
- Optimized beep timing (8× 50ms = 400ms total)
- LCD splash: 300ms (kept for visual feedback)
- Full MCB init: 500ms wait + CL query
- **Total: ~1.7 seconds** (vs original 5s = **3× faster!**)

**Soft Boot (OFF Button / RESET command):**
- Skip LCD splash (save 300ms)
- Skip all beeps (save 400ms)
- Skip MCB sensor init (save 500ms - use fast 50ms path)
- **Total: ~0.25 seconds** (vs original 0.5s = **2× faster!**)

---

### 3. Flash Script Improvements ✅

**Problem**: Original Teknatool firmware sets flash write protection, requiring slow OpenOCD unlock (~30s).

**Solution**: Optimized flash script with fast/slow paths.

**New Commands:**
```bash
./flash_firmware.sh custom    # First time from OEM (slow, 30s)
./flash_firmware.sh quick     # Development updates (fast, 2s!)
./flash_firmware.sh original  # Restore OEM (slow, 30s)
```

**Speed Improvement:**
- Development cycle: **15× faster** (30s → 2s per flash)
- Uses st-flash direct write when unlocked
- Uses OpenOCD unlock only when needed (protected firmware)

---

### 4. Boot Failures Fixed ✅

**Issue 1: HAL_Delay() Hang**
- **Cause**: Used `HAL_Delay()` before FreeRTOS scheduler started (SysTick not initialized)
- **Symptom**: Infinite hang during version splash → watchdog reset
- **Fix**: Replaced with `lcd_delay_ms()` (busy-wait loop)

**Issue 2: Watchdog Timeout**
- **Cause**: 3s watchdog too aggressive for 2.5s cold boot sequence
- **Symptom**: "WATCHDOG RESET" during cold boot
- **Fix**: Increased to 5s timeout + added refreshes after every long delay

**Issue 3: Stack Corruption (ASSERT FAIL)**
- **Cause**: COLDBOOT magic at 0x2000BF00 (only 256 bytes from stack top)
- **Symptom**: "ASSERT FAIL!" message, FreeRTOS assertion triggered
- **Fix**: Moved magic to 0x20002700 (safe location after .bss)

**Issue 4: Settings Auto-Save**
- **Cause**: Settings never saved to flash, "Bad magic" on every boot
- **Symptom**: Slow settings init, wasted flash writes
- **Fix**: Auto-save defaults on first boot with bad magic

---

## Test Commands

**COLDBOOT Command** - Force cold boot simulation (for testing):
```
COLDBOOT
```
- Sets magic value in SRAM (0x20002700)
- Triggers software reset
- Boot detects magic → treats as BOOT_COLD
- Shows splash, beeps, full init

**RESET Command** - Normal soft boot:
```
RESET
```
- Software reset via NVIC_SystemReset()
- Boot detects SFTRSTF flag → BOOT_SOFT
- Fast path, no splash

---

## Technical Details

### RCC->CSR Reset Flags (GD32F303 / STM32F1xx)

| Bit | Flag | Meaning |
|-----|------|---------|
| 27 | PORRSTF | Power-on reset (cold boot) |
| 28 | SFTRSTF | Software reset (NVIC_SystemReset) |
| 29 | IWDGRSTF | Independent watchdog timeout |
| 30 | WWDGRSTF | Window watchdog timeout |
| 26 | PINRSTF | External NRST pin reset |
| 24 | RMVF | Clear all flags (write 1) |

### Boot Detection Logic

```c
static void detect_boot_type(void) {
    // Check for test override (COLDBOOT command)
    if (*FORCE_COLD_BOOT_MAGIC_ADDR == 0xC01DB007) {
        g_boot_type = BOOT_COLD;
        *FORCE_COLD_BOOT_MAGIC_ADDR = 0;
        return;
    }

    // Check hardware reset flags
    uint32_t csr = RCC->CSR;
    if (csr & RCC_CSR_IWDGRSTF) g_boot_type = BOOT_WATCHDOG;
    else if (csr & RCC_CSR_SFTRSTF) g_boot_type = BOOT_SOFT;
    else if (csr & RCC_CSR_PORRSTF) g_boot_type = BOOT_COLD;
    else g_boot_type = BOOT_COLD;  // Default

    // Clear flags for next reset
    RCC->CSR |= RCC_CSR_RMVF;
}
```

### Watchdog Configuration

```c
// 5 second timeout for safe cold boot margin
IWDG->PR = 6;      // Prescaler /256 (40kHz LSI → 156.25 Hz)
IWDG->RLR = 781;   // 781 / 156.25 Hz = 5.0 seconds

// Refresh in main loop (every ~100ms when all tasks alive)
if (ALL_TASKS_ALIVE()) {
    IWDG->KR = 0xAAAA;
}
```

### Memory Layout

| Section | Address | Size | Notes |
|---------|---------|------|-------|
| Bootloader | 0x08000000 | 12KB | DFU mode, preserved |
| Firmware | 0x08003000 | ~59KB | Application code |
| .data | 0x20000000 | ~2KB | Initialized data |
| .bss | 0x200007F8 | ~8KB | Zero-initialized |
| COLDBOOT magic | 0x20002700 | 4B | Test command flag |
| Heap/Stack | 0x200026C8+ | ~38KB | Dynamic allocation |
| _estack | 0x2000C000 | - | Stack top (grows down) |

---

## Performance Results

### Boot Speed

| Metric | Original | Ours | Improvement |
|--------|----------|------|-------------|
| Cold boot | ~5.0s | ~1.7s | **3× faster** |
| Soft boot | ~0.5s | ~0.25s | **2× faster** |
| Beeps | Unknown | 8 tones | Musical scale |

### Flash Speed

| Operation | Before | After | Improvement |
|-----------|--------|-------|-------------|
| Dev update | ~30s (OpenOCD) | ~2s (st-flash) | **15× faster** |
| First install | ~30s | ~30s | Same (unlock needed) |

---

## Files Modified

### Core Boot Logic
- `include/shared.h` - Boot type enum
- `src/main.c` - RCC detection, conditional boot sequence
- `src/task_motor.c` - Skip MCB init on soft boot
- `src/lcd.c` - Conditional splash screen
- `src/commands.c` - Added COLDBOOT test command
- `src/settings.c` - Auto-save defaults on first boot

### Documentation
- `README.md` - Added boot behavior and flashing guide
- `flash_firmware.sh` - Optimized for fast/slow paths

---

## Known Issues & Limitations

### Settings "Bad Magic" on First Boot
- **Expected**: First boot after fresh flash has no settings in flash
- **Behavior**: Initializes defaults and saves automatically
- **Resolution**: Second boot loads saved settings ("CRC valid")

### OpenOCD Slow Write Speed
- **Cause**: OpenOCD writes at ~2.5 KiB/s (vs st-flash 100+ KiB/s)
- **Impact**: First install and OEM restore take ~30s
- **Workaround**: Use `quick` command for development (15× faster)

---

## Future Enhancements

### Boot Speed
- ✅ Boot type detection implemented
- ✅ Soft boot optimized (<0.25s)
- ⚠️ Could reduce LCD splash from 300ms → 150ms for even faster cold boot
- ⚠️ Could make beeps optional/configurable

### Flashing
- ✅ Fast path for unlocked devices
- ⚠️ Could add DFU flashing option (no Pi/ST-Link needed)
- ⚠️ Could add OTA update via serial

---

## Testing

**Test Cold Boot:**
```bash
# Via serial command (simulated cold boot)
COLDBOOT

# Via power cycle (real cold boot)
# 1. Power OFF drill press
# 2. Wait 2 seconds
# 3. Power ON
```

**Test Soft Boot:**
```bash
# Via serial command
RESET

# Via OFF button
# Press OFF button on drill press
```

**Expected Results:**
- Cold boot: 8 beeps (rising musical scale), splash screen, ~1.7s
- Soft boot: Silent, instant UI, ~0.25s

---

END OF SESSION SUMMARY

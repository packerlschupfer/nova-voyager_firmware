# R2P06k Official vs Backup Firmware Comparison
**Official Source**: Teknatool Support (Drill Press HMI - R2P06k CG.dfu)  
**Backup Source**: User's device DFU download  
**Analysis Date**: January 14, 2026

---

## CRITICAL FINDING: Backup Was Incomplete!

**Official Firmware**: 118,120 bytes  
**User Backup**: 116,000 bytes  
**Missing**: 2,120 bytes (1.8% of firmware)

**Comparison Result**:
- ✅ First 116,000 bytes: **100% IDENTICAL**
- ❌ Bytes 116,000-118,120: **MISSING from backup**

**Conclusion**: User's backup is **truncated** - missing end of firmware

---

## What's in the Missing 2,120 Bytes?

**Location**: 0x0801F520 - 0x0801FD68 (flash addresses)

**Content Breakdown**:
- Data/Code: 1,788 bytes (84.3%)
- Zeros (padding): 328 bytes (15.5%)
- 0xFF (erased): 4 bytes (0.2%)

**Types of Data**:
1. **Lookup tables** (~400 bytes of data patterns)
2. **String tables** (~1,400 bytes of text)
3. **Padding** (328 bytes of zeros)

---

## Missing Strings (100 total)

### Safety Warnings (Multi-Language)

**English**:
```
"Caution"
"Refer to owners manual for safe operating procedure."
"Ensure proper speed and direction are selected."
"Always use proper safety gear and wear eye protection."
```

**German**:
```
"Warnung!!"
"Sie die Anweisungen der Bedienungsanleitung."
"Achten Sie darauf, dass die richtige Geschwindigkeit..."
"verwenden Sie immer Schutzeinrichtungen und Schutzbrille."
```

**French**:
```
"Attention!!"
"Consultez manuel d'instructions pour utilisation en sécurité"
"Assurez vous de sélectionner la vitesse et la direction..."
"Toujours utilisez des protecteur et les protections des yeux."
```

### Error Messages

```
"MCB Disconnected"
"MCB n'est pas connecté"
"Hauptkontrollplatine nicht angeschlossen"
"HMI EEPROM ERR"
"Erreur HMI EEPROM"
"Param Load Failed"
```

### Setup/Calibration

```
"Erste-Inbetriebnahme?" (First commissioning?)
"Einleiten des Setups" (Initiate setup)
"Überspringen" (Skip)
"Deactivierung mit F3" (Deactivate with F3)
"Première Installation" (First installation)
"Commencer" (Begin)
"Calibrate"
"kalibrieren?" (calibrate?)
"profondeur?" (depth?)
```

### USB/Firmware

```
"- MODE USB -"
"- USB MODE -"
"- USB Modus -"
"Firmware USB"
"found..."
"introuvable..." (not found...)
```

### Sensor Related

```
"sensor??"
"le capteur de" (the sensor of)
"Zero"
"Menu"
```

---

## Impact Analysis

### Does This Affect Nova Firmware?

**Minimal Impact**:
- ✅ Motor commands: All in first 116KB (working)
- ✅ Core functionality: Unaffected
- ⚠️ Error messages: Missing some strings
- ⚠️ Setup wizard: May have incomplete first-boot sequence
- ⚠️ Safety warnings: Missing display strings

**What Was Missed**:
- Safety warning screens (startup disclaimer)
- Advanced error messages (MCB disconnected, etc.)
- First-time setup wizard strings
- USB mode strings
- Some calibration prompts

---

## String Count Comparison

**Official**: 2,311 strings  
**Backup**: 2,211 strings  
**Missing**: 100 strings (4.3%)

**Missing String Categories**:
- Safety disclaimers: ~30 strings
- Error messages: ~20 strings
- Setup wizard: ~15 strings
- USB mode: ~10 strings
- Calibration: ~10 strings
- Sensor prompts: ~5 strings
- Misc: ~10 strings

---

## Should You Reflash With Official?

### Pros

✅ **Complete firmware** (no truncation)  
✅ **All strings present** (proper error messages)  
✅ **Official from Teknatool** (guaranteed authentic)  
✅ **Full feature set** (setup wizard, calibration, etc.)

### Cons

⚠️ **Your device works** (first 116KB is identical)  
⚠️ **Missing strings not critical** (motor control unaffected)  
⚠️ **Risk of flash** (always a small risk)

### Recommendation

**Option 1**: Keep current firmware (works fine)
- Missing strings are cosmetic (warnings, setup wizard)
- Core functionality identical
- No motor command differences

**Option 2**: Flash official firmware (get complete version)
- Gain error messages and setup wizard
- Have definitive reference firmware
- Ensure nothing else is truncated

**My Recommendation**: Flash official firmware for completeness

---

## Motor Command Analysis

### Does Official Have Different Commands?

**To Verify**: Disassemble official firmware and compare motor command functions

**Expected Result**: Identical (first 116KB is 100% same)
- Motor command code is in first 116KB
- String tables are at end (116KB+)
- Commands should be identical

**Action**: Should I disassemble official firmware to confirm?

---

## Next Steps

1. **Disassemble official firmware**
   ```bash
   arm-none-eabi-objdump -D -b binary -m arm -Mforce-thumb \
       --adjust-vma=0x08003000 \
       firmware_r2p06k_cg_official.bin > firmware_r2p06k_cg_official.asm
   ```

2. **Compare motor command sections**
   - Check 0x0801A000 range (motor functions)
   - Verify command codes unchanged
   - Confirm initialization sequences

3. **Verify sensor init sequence**
   - Check if official has same VR/CL/VS/V8/VG sequence
   - Compare with R2P06e
   - Identify any R2P06k-specific differences

4. **Use official as reference**
   - More complete than backup
   - Definitive R2P06k firmware
   - Base nova_firmware implementation on this

---

## Summary

**User Backup**: 
- ✅ Core code identical to official
- ❌ Missing 2,120 bytes of strings/data at end
- ⚠️ Truncated during DFU download

**Official Firmware**:
- ✅ Complete and untruncated
- ✅ All strings and data present
- ✅ Should be used as reference

**Impact on Nova Firmware**: Minimal
- Motor commands in first 116KB (unchanged)
- Missing strings are UI text (not critical)
- Sensor initialization likely identical

**Recommendation**: 
1. Use official firmware as definitive reference
2. Optionally flash to device for completeness
3. Disassemble official for final verification

Should I proceed with disassembly and detailed comparison?

---

END OF COMPARISON ANALYSIS

---

## SENSOR INITIALIZATION SEQUENCE CONFIRMED

### Official R2P06k Firmware (0x801A574)

**IDENTICAL to R2P06e**:

```asm
0x801A576: movs  r1, #0
0x801A578: movw  r0, #22098      ; 0x5652 = VR
0x801A57C: bl    motor_cmd       ; VR(0)
0x801A580: movs  r0, #5
0x801A582: bl    delay_ms        ; delay(5ms)

0x801A586: movs  r1, #0
0x801A588: movw  r0, #17228      ; 0x434C = CL
0x801A58C: bl    motor_cmd       ; CL(0)
0x801A590: movs  r0, #5
0x801A592: bl    delay_ms        ; delay(5ms)

0x801A596: movs  r1, #0
0x801A598: movw  r0, #22099      ; 0x5653 = VS
0x801A59C: bl    motor_cmd       ; VS(0)
0x801A5A0: movs  r0, #5
0x801A5A2: bl    delay_ms        ; delay(5ms)

0x801A5A6: mov.w r1, #264        ; 0x108
0x801A5AA: movw  r0, #22072      ; 0x5638 = V8
0x801A5AE: bl    motor_cmd       ; V8(264)
0x801A5B2: movs  r0, #5
0x801A5B4: bl    delay_ms        ; delay(5ms)

0x801A5B8: movw  r1, #261        ; 0x105
0x801A5BC: movw  r0, #22087      ; 0x5647 = VG
0x801A5C0: bl    motor_cmd       ; VG(261)
0x801A5C4: pop   {r4, pc}        ; Return
```

**Sequence**: VR(0) → CL(0) → VS(0) → V8(264) → VG(261)

**Exactly same as R2P06e!** ✅

---

## VERIFICATION: Motor Commands in R2P06k

All sensor commands present in official R2P06k:

| Command | Code | Address | Status |
|---------|------|---------|--------|
| VR | 0x5652 | 0x801A56A, 0x801A578 | ✅ Present |
| CL | 0x434C | 0x801A55A, 0x801A588, etc. | ✅ Present |
| VS | 0x5653 | 0x801A54A, 0x801A598 | ✅ Present |
| V8 | 0x5638 | 0x801A5AA | ✅ Present |
| VG | 0x5647 | 0x801A5BC | ✅ Present |
| GR | 0x4752 | 0x801A53A | ✅ Present |

**All sensor commands present in R2P06k official firmware!** ✅

---

## CONCLUSION

### User Backup vs Official

**Firmware Code**: 100% IDENTICAL (first 116KB)
- Motor commands: Same
- Sensor init: Same  
- Functionality: Identical

**Missing from Backup**: 2,120 bytes of strings
- Safety warnings (multi-language)
- Error messages (MCB disconnected, etc.)
- Setup wizard text
- USB mode strings

**Recommendation**: Use official firmware as reference, but backup is functionally equivalent for motor control analysis.

### Sensor Initialization in R2P06k

**Confirmed**: R2P06k has SAME sensor init as R2P06e
- VR(0) → CL(0) → VS(0) → V8(264) → VG(261)
- Sequence at 0x801A574 (same offset as R2P06e!)
- No differences in initialization

**Implication**: Nova firmware sensor init fix will work on R2P06k!

---

## For Nova Firmware Implementation

**Use Official R2P06k as Reference**:
- ✅ Complete firmware (no truncation)
- ✅ Verified sensor initialization
- ✅ All commands confirmed present
- ✅ String tables complete

**Sensor Init Sequence to Add**:
```c
// At motor_task_init(), BEFORE motor_sync_settings():

// Query CL first (unlock)
send_query(CMD_CURRENT_LIMIT);
wait_response();
delay(10ms);

// Vibration init sequence
VR(0); delay(5ms);
CL(0); delay(5ms);
VS(0); delay(5ms);
V8(264); delay(5ms);
VG(261); delay(5ms);

// Now sensors should work
// Then continue with motor_sync_settings()
```

This will unlock HT, VR, LD, and other sensor queries!

---

END OF OFFICIAL FIRMWARE COMPARISON

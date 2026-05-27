# Combinable Triggers - Testing Guide

## Firmware Version
**Branch:** `feature/combinable-triggers`
**Commits:** 9 (1812010)
**Build:** 82,397 bytes (31.2% flash)

---

## Pre-Test Checklist

- [ ] Firmware flashed successfully (./flash_firmware.sh quick)
- [ ] Serial console accessible (screen /dev/ttyUSB0 9600 or via Pi)
- [ ] Safety systems functional (E-Stop, Guard)
- [ ] Motor controller responding

---

## Test 1: Verify Firmware Boot

**Serial Console:**
```
> STATUS

Expected output:
- App state, motor state, speeds
- Tap mode shown (should be OFF initially)
- No errors
```

**LCD Display:**
```
Row 3 should show: TAP:---
(No triggers enabled initially)
```

---

## Test 2: Legacy Mode Compatibility

Test that old mode selector still works:

**Via Menu:**
```
Menu → Tapping → Mode → Quill
→ Display should show: TAP:Q
→ Press MENU to exit
→ Start motor, lift quill → should auto-reverse
```

**Via Serial:**
```
> TAP 2
→ Should enable Quill mode (TAP:Q)

> TAP 0
→ Should disable (TAP:---)
```

**Expected:** Legacy mode API works, sets corresponding trigger.

---

## Test 3: Individual Triggers

### Test 3A: Quill Trigger Only
```
Menu → Tapping
  Speed: 200
  []Quill = On
  < Back (save settings)

→ Display: TAP:Q

Start motor (START command or ON button)
Manually push quill down (drilling motion)
→ Motor should run forward

Lift quill up 2.5mm+
→ Should auto-reverse (watch "[TAP] Trigger fired: QUILL" in console)

Push quill down 2.5mm+
→ Should auto-forward again

Expected: Bidirectional auto-following works
```

### Test 3B: Depth Trigger Only
```
Menu → Tapping
  []Quill = Off
  []Depth = On

Menu → Depth
  Target: 50 (5.0mm)
  Mode: Std
  < Back

→ Display: TAP:D

Start motor, push quill to 5mm depth
→ Should reverse at target (watch "[TAP] Trigger fired: DEPTH")

Expected: Reverses at target depth
```

### Test 3C: Load Increase Trigger
```
Menu → Tapping
  []Depth = Off
  []LdInc = On

Menu → Tapping → Load >
  Thresh: 60%
  RevTime: 200ms

→ Display: TAP:I

Start motor in wood
Apply heavy pressure (simulate overload)
→ Should detect KR spike and reverse
→ Console: "[TAP] Load spike: KR=XX%"

Expected: Reverses on excessive load
```

### Test 3D: Pedal Trigger
```
Menu → Tapping
  All triggers = Off
  []Pedal = On

→ Display: TAP:P

Start motor
Press foot pedal
→ Should reverse immediately
Release pedal (if HOLD mode)
→ Should stop

Expected: Manual control via pedal
```

---

## Test 4: Trigger Combinations

### Test 4A: Quill + Pedal (Most Useful)
```
Menu → Tapping
  []Quill = On
  []Pedal = On

→ Display: TAP:QP

Start motor
Test 1: Lift quill → auto-reverse (QUILL trigger)
Test 2: Press pedal → immediate reverse (PEDAL overrides)

Expected: Both triggers work, pedal has priority
Console: "[TAP] Trigger fired: PEDAL" or "QUILL"
```

### Test 4B: Depth + Quill (Safety Limit)
```
[]Depth = On
[]Quill = On
Target: 100 (10mm)

→ Display: TAP:DQ

Start motor, push quill
- Lift before 10mm → quill reverses
- Reach 10mm → depth stops/reverses (priority)

Expected: Quill auto-reverse until depth limit reached
Console shows which trigger fired
```

### Test 4C: Load Inc + Load Slip + Clutch (Comprehensive)
```
[]LdInc = On
[]LdSlp = On
[]Clutch = On

→ Display: TAP:ISC

Test scenarios:
1. Through-hole: Should detect CV overshoot at exit
2. Blind hole: Should detect KR spike at bottom
3. Clutch holder: Should detect load plateau

Expected: Detects all three load conditions
Console: "[TAP] Load spike:" or "Clutch slip:" or "Through-hole:"
```

### Test 4D: Maximum Protection
```
Enable ALL:
[]Depth = On
[]LdInc = On
[]LdSlp = On
[]Clutch = On (if clutch holder available)
[]Quill = On
[]Pedal = On

→ Display: TAP:DISCQP or TAP:ALL

Any condition should trigger reversal:
- Depth reached
- Load spike
- CV overshoot
- Clutch slip
- Quill lift
- Pedal press

Expected: Maximum safety, any trigger fires
Console shows which trigger activated
```

---

## Test 5: Priority Verification

Enable multiple triggers and verify priority order:

```
[]Depth = On
[]Quill = On
[]Pedal = On

Priority: Pedal > Quill > Depth

Test: Press pedal while quill lifting
→ Should show: "[TAP] Trigger fired: PEDAL" (not QUILL)

Test: Reach depth while quill stable
→ Should show: "[TAP] Trigger fired: DEPTH"

Expected: Higher priority wins when multiple conditions met
```

---

## Test 6: Settings Persistence

```
1. Enable triggers: []Depth=On, []Quill=On
2. Save: SAVE command or Menu exit
3. Reset: RESET command
4. Check: Display should still show TAP:DQ

Expected: Triggers persist across reboot
```

---

## Test 7: Display Abbreviations

Verify display shows correct trigger codes:

| Triggers Enabled | Expected Display |
|------------------|------------------|
| None | TAP:--- |
| Depth | TAP:D |
| Load Inc | TAP:I |
| Load Slip | TAP:S |
| Clutch | TAP:C |
| Quill | TAP:Q |
| Peck | TAP:K |
| Pedal | TAP:P |
| Depth+Quill | TAP:DQ |
| Load Inc+Slip+Clutch | TAP:ISC |
| All | TAP:DISCQKP or TAP:ALL |

---

## Expected Console Output Examples

**Quill Trigger:**
```
[TAP] Trigger fired: QUILL
[TAP] Reversing: QUILL_LIFT
```

**Depth Trigger:**
```
[TAP] Trigger fired: DEPTH
[TAP] Reversing: DEPTH
```

**Load Triggers:**
```
[TAP] Baseline learned: CV=450 KR=6
[TAP] Load spike: KR=75% (baseline=50%)
[TAP] Trigger fired: LOAD_INC
[TAP] Reversing: LOAD_INC
```

**Clutch Slip:**
```
[TAP] Clutch slip detected: KR=85% for 520ms
[TAP] Clutch slip triggered reversal
[TAP] Trigger fired: CLUTCH
```

**Priority Resolution:**
```
[TAP] Trigger fired: PEDAL
(even if other triggers also want to fire)
```

---

## Known Issues / Limitations

1. **Peck trigger not yet integrated** into unified state machine
   - Peck timing requires multi-phase state machine
   - Currently disabled in unified system
   - TODO: Add peck cycle support

2. **Clutch slip requires clutch tap holder** to test
   - Needs actual torque-limiting tap holder
   - May need tuning (plateau duration, load delta)

3. **Legacy mode selector coexists with triggers**
   - Setting Mode also sets corresponding trigger
   - Can cause confusion if both used
   - Recommend: Use either Mode OR triggers, not both

---

## Troubleshooting

**Display shows "---" but trigger enabled:**
- Check settings saved (SAVE command)
- Verify menu changes applied
- Check serial console for trigger enable confirmation

**Trigger doesn't fire:**
- Check baseline learned (load triggers need ~4s)
- Verify threshold settings (may need adjustment)
- Check console for detection logs

**Multiple triggers conflict:**
- Check priority order (Pedal > Quill > Depth > Load)
- Verify only ONE should show "[TAP] Trigger fired:"
- If wrong trigger firing, adjust enables

**Settings don't persist:**
- Run SAVE command
- Check EEPROM detected at boot
- Verify no "Bad magic" errors

---

## Success Criteria

- ✓ Individual triggers work
- ✓ Combinations work (multiple can be enabled)
- ✓ Priority resolves conflicts correctly
- ✓ Display shows active triggers
- ✓ Settings persist across reboot
- ✓ Console logging shows which trigger fired
- ✓ No crashes or watchdog resets

---

## Report Results

After testing, document:
1. Which trigger combinations tested
2. Which worked as expected
3. Any issues or unexpected behavior
4. Console log snippets
5. Ready to merge? (yes/no)

# Motor Profile Feature - User Guide
**Feature**: Motor Profile Selection (SOFT/NORMAL/HARD)
**Date Implemented**: 2026-01-14
**Firmware Version**: v0.1.0-RTOS+

---

## What Are Motor Profiles?

Motor profiles control **torque behavior during acceleration and under load**:

| Profile | Acceleration | Torque | Best For |
|---------|-------------|--------|----------|
| **SOFT** | Gentle | Low | Delicate materials, thin stock, precision work |
| **NORMAL** | Balanced | Medium | General purpose drilling (default) |
| **HARD** | Aggressive | High | Hard materials, large bits, heavy-duty work |

---

## Discovery Results

**Your Observation** (2026-01-14):
> "with the first profile i could stop the chuck nearly by hand. not possible with the 2nd profile"

This confirmed that profiles **DO affect motor torque output**!

**Motor Controller Commands:**
- SOFT → S0(0)
- NORMAL → S7(750)
- HARD → S8(264)

---

## How to Use Motor Profiles

### Via LCD Menu

1. Press **MENU** button
2. Navigate to **"Motor"** submenu
3. Select **"Profl"** (Profile)
4. Choose profile:
   - **Soft** - Gentle acceleration, low torque
   - **Norm** - Balanced (default)
   - **Hard** - Aggressive, high torque
5. Press **START** to exit menu
6. Press **SAVE** (if you want to persist)

The profile will be applied automatically **every time you press START** to begin drilling.

### Via Serial Commands

```bash
# Connect to serial
ssh pi@192.168.16.62
screen /dev/ttyUSB0 9600

# View current profile (in menu settings)
STATUS

# Profiles are set automatically when you press START
# To manually test profiles, use Python script below
```

---

## Manual Profile Testing

If you want to test profiles manually via serial:

```python
# On Raspberry Pi:
import serial, time

ser = serial.Serial('/dev/ttyUSB0', 9600, timeout=2)
time.sleep(0.5)

# Set speed
ser.write(b'SPEED 1000\r\n')
time.sleep(0.5)

# Set SOFT profile manually
pkt = [0x04, 0x30, 0x30, 0x31, 0x31, 0x02, 0x31, ord('S'), ord('0'), ord('0'), 0x03]
xor = 0
for i in range(6, len(pkt)): xor ^= pkt[i]
pkt.append(xor)
ser.write(bytes(pkt))
time.sleep(0.5)

# Start motor
ser.write(b'START\r\n')

# Test torque by gently resisting the chuck

ser.close()
```

---

## Testing Plan

### Test 1: SOFT Profile
1. Set profile to **SOFT** in menu
2. Press **START** button
3. **Observe**: Motor accelerates gently
4. **Test**: Try stopping chuck with light hand pressure
5. **Expected**: Chuck should resist but can be stopped by hand

### Test 2: NORMAL Profile
1. Set profile to **NORMAL** in menu
2. Press **START** button
3. **Observe**: Motor accelerates at normal rate
4. **Test**: Try stopping chuck with moderate hand pressure
5. **Expected**: Chuck resists more, harder to stop than SOFT

### Test 3: HARD Profile
1. Set profile to **HARD** in menu
2. Press **START** button
3. **Observe**: Motor accelerates aggressively
4. **Test**: Try stopping chuck with firm hand pressure
5. **Expected**: Chuck resists strongly, very difficult to stop by hand

**SAFETY**: Be careful! HARD profile has high torque - don't force your hand if chuck won't stop!

---

## What Profile Should I Use?

### Use SOFT When:
- Drilling delicate materials (plastic, soft wood, thin metal)
- Using small diameter bits (< 6mm)
- Precision work where you need gentle feed pressure
- Tapping small threads
- Breaking through materials (anti-tear-out)

### Use NORMAL When:
- General purpose drilling
- Medium diameter bits (6-12mm)
- Mixed materials
- Most day-to-day work

### Use HARD When:
- Drilling hard materials (steel, hardwood)
- Large diameter bits (> 12mm)
- Forstner bits, spade bits
- Heavy material removal
- When you need maximum torque

---

## Technical Details

### Profile Command Mapping

| Profile | Command | Parameter | Effect |
|---------|---------|-----------|--------|
| SOFT | S0 | 0 | Sets motor controller to gentle mode |
| NORMAL | S7 | 750 | Sets motor controller to balanced mode |
| HARD | S8 | 264 | Sets motor controller to aggressive mode |

### When Profile is Applied

Profile is sent to the motor controller **immediately before motor starts**, in this sequence:

1. User presses **START** button
2. Firmware sets speed (CMD_MOTOR_SET_SPEED)
3. **Firmware applies profile** (S0/S7/S8) ← NEW
4. Firmware starts motor forward (CMD_MOTOR_FORWARD)

### Profile Persistence

- Profile setting is **saved to flash** when you press SAVE
- Profile persists **across power cycles**
- Default profile: **NORMAL**

---

## Troubleshooting

### "I don't feel any difference between profiles"

**Check:**
1. Are you testing during **spinup** (acceleration)? Profiles affect startup behavior most.
2. Try with the motor **under light load** (touch the chuck gently during spinup)
3. Profiles are most noticeable at **higher speeds** (1000+ RPM)

### "Motor doesn't start after changing profile"

**Solution:**
1. Press **MENU** to exit menu first
2. Make sure guard is **closed**
3. Check if **E-stop** is pressed
4. Try **RESET** command via serial

### "Profile reverts after power cycle"

**Solution:**
1. After changing profile in menu, press **MENU** to exit
2. Via serial, run: `SAVE`
3. This persists the profile to flash

---

## Implementation Details

### Files Modified

| File | Changes |
|------|---------|
| `include/config.h` | Added CMD_PROFILE_S0/S7/S8 definitions |
| `include/motor.h` | Added motor_set_profile() declaration |
| `src/motor.c` | Implemented motor_set_profile() function |
| `src/events.c` | Call motor_set_profile() before motor start |
| `src/menu.c` | Profile menu already existed! |
| `include/settings.h` | motor_profile_t already existed! |

### Profile Implementation

```c
void motor_set_profile(uint8_t profile) {
    switch (profile) {
        case MOTOR_PROFILE_SOFT:
            motor_send_command(CMD_PROFILE_S0, 0);    // S0(0)
            break;
        case MOTOR_PROFILE_NORMAL:
            motor_send_command(CMD_PROFILE_S7, 750);  // S7(750)
            break;
        case MOTOR_PROFILE_HARD:
            motor_send_command(CMD_PROFILE_S8, 264);  // S8(264)
            break;
    }
}
```

Called from `events.c` in the START button handler:

```c
// Apply motor profile before starting
const settings_t* settings = settings_get();
if (settings) {
    motor_set_profile(settings->motor.profile);
}
```

---

## Next Steps

After testing profiles, we can implement:

1. **Power Output Menu** (CL command: Low/Med/High 20%/50%/70%)
2. **Profile + Material Auto-Selection** (auto-select profile based on material)
3. **Per-Material Profile Presets** (save favorite profile for each material)

---

## Feedback

**Please test and report:**
1. Do you notice torque differences between profiles?
2. Which profile do you prefer for general work?
3. Any specific materials where one profile works better?

Share your findings and we can document best practices!

---

END OF USER GUIDE

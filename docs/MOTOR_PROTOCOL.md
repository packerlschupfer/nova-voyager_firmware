# Motor Communication Protocol Reference
## Motor Communication Protocol

The HMI (GD32F303) communicates with a **separate motor controller** via serial.

### Hardware Connection (Verified)
- **UART**: USART3 on PB10 (TX) / PB11 (RX)
- **Baud**: 9600, 8N1
- **Response**: ACK (0x06) for success, NAK (0x15) for error

### Protocol Formats (Two Different Formats!)

**1. QUERY Format** (for reading status/parameters like GF):
```
[0x04][0x30][0x30][0x31][0x31][0x31][CMD_H][CMD_L][0x05]
 SOH   '0'   '0'   '1'   '1'   '1'   Command       ENQ
```
- Position 5 is '1' (0x31) NOT STX!
- Ends with ENQ (0x05) NOT ETX!
- NO checksum!

**2. COMMAND Format** (for motor control commands):
```
[0x04][0x30][0x30][0x31][0x31][0x02][0x31][CMD_H][CMD_L][PARAM...][0x03][XOR]
 SOH   '0'   '0'   '1'   '1'   STX  '1'   Command     Parameter    ETX  Checksum
```
- Position 5 is STX (0x02)
- Position 6 is '1' (unit byte)
- **XOR checksum starts from position 6 (unit byte '1'), NOT from STX!**
- Example RS stop: `04 30 30 31 31 02 31 52 53 30 03 03`

### Commands
| Command | Code | Parameter | Description |
|---------|------|-----------|-------------|
| RS | 0x5253 | 0 | Motor stop/brake - **confirmed via TX capture** |
| JF | 0x4A46 | 1706/1707 | Set direction - **1706=FORWARD, 1707=REVERSE** |
| JF | 0x4A46 | 3670/3669 | Jog mode - **3670=JOG_START, 3669=JOG_END** (see below) |
| ST | 0x5354 | 0 | Motor start/enable - **confirmed via TX capture** |
| SV | 0x5356 | RPM | Set Velocity - target speed |
| **CV** | **0x4356** | **RPM** | **Current Velocity - actual motor speed (feedback)** |
| GF | 0x4746 | 32/34/436/438 | Get Flags - see table below |
| **KR** | **0x4B52** | **0-100** | **Keep Running / Load% - 0=stopped, 100=startup, 10-11=unloaded, 13-15=accel** |
| **S2** | **0x5332** | **900** | **Speed 2 - secondary speed param, always 900** |
| **CL** | **0x434C** | **70/100** | **Current Limit - 70% idle, 100% running** |
| **GV** | **0x4756** | **version** | **Get Version - returns MCB firmware (e.g., "B1.7")** |
| **VR** | **0x5652** | **0/100** | **Voltage Ramp - 0=off, 100=full (spindle hold)** |
| **VS** | **0x5653** | **0/1** | **Voltage Set - 0=off, 1=on (spindle hold enable)** |
| **V8** | **0x5638** | **264** | **Voltage Param 8 (spindle hold config)** |
| **VG** | **0x5647** | **261** | **Voltage Gain (spindle hold config)** |
| **SL** | **0x534C** | **10** | **Speed Limit (during spindle hold)** |

### GF Status Flags (discovered 2026-01-24)
| Value | Meaning |
|-------|---------|
| 32 | STOPPED (forward mode) |
| 34 | RUNNING (forward mode) |
| 436 | STOPPED (reverse mode) |
| 438 | RUNNING (reverse mode) |
| 16929+ | ERROR state (bit 14 = 0x4000 set) |

**GF Error Detection**: When bit 14 (0x4000) is set, MCB is in error state.
Query `F0` to get fault code. Example: GF=16929 → F0? → F0=13 (Under Voltage Low)

### Motor Power Output (CL mapping, discovered 2026-01-25)

The UI "Motor Power" setting maps to CL (current limit):

| UI Setting | CL Value | Notes |
|------------|----------|-------|
| Low | 20% | May not start at low RPM! |
| Med | 50% | |
| High | 70% | Factory default |
| MAX | 100% | Full torque |

**Parameter commit sequence:**
```
CL=20       → Set current limit value
SE=CL       → Commit/apply the parameter (0x5345 = "SE")
CL?         → Verify it took
```

**SE command (0x5345):** "Set Enable" - commits parameter changes. Without this,
parameter changes may not take effect.

**Low power stall condition:** At CL=20% with low RPM (e.g., SV=50), the motor
may not have enough torque to overcome static friction:
- GF=34 (RUN) but CV=0 (not actually spinning)
- KR=0 (current limiting prevents real current draw)
- After ~2s, MCB may return garbage (0xFF) - possible stall/watchdog fault
- RS=0 recovers to normal stopped state

### Brake, Spindle Hold, and Buzzer — Decoded 2026-05-26

#### Brake (BR command)
**OEM never writes BR.** Zero occurrences of 0x4252 (BR command code) anywhere
in the firmware in any encoding. Stop is **always** `RS=0` via wrapper at 0x0801A456.
No coast-vs-brake user setting. The MCB's own brake mode (a factory-set BR value)
controls deceleration regardless. Our REGSCAN showing BR=0 = MCB factory default,
untouched.

For our firmware: BR is available to set but we have no reason to. Leave it at
factory default and use RS=0 for all stops.

#### Spindle Hold
**Hardcoded 30-second timeout, user-triggered only, no auto-engage.**

- **UI entry**: "Powered Spindle Hold" screen at 0x0800AE48
- **Default force**: CL=15 (literal at 0x0800AE4C: `movs r4,#15`)
- **Force range**: -30 (loosen) to +25 (tighten), encoder-adjustable
- **Timeout**: 30 seconds (`movs r6,#30` literal — NOT in EEPROM)
- **Trigger**: User navigates to menu, presses ON to engage. **Never auto-triggered
  on motor stop** — no caller in any motor-stop path.

**Engage sequence** (function 0x0801A574 → 0x0801A5C6):
```
VR=0, delay 5ms, CL=0, delay 5ms, VS=0, delay 5ms, V8=264, delay 5ms, VG=261
loop while not exiting:
    VR=100, CL=<force>, VS=1
    poll keys (encoder for force adjust, ON/OFF/timeout to exit)
```

**Release**: standard `RS=0` — no special "VR=0/VS=0 un-hold" sequence.

#### Buzzer / Beeper
**Pure software bit-bang** (NOT timer PWM). All beeps via tone generator at
0x080185BC with signature `tone(freq_hz, duration_ms)`.

**Hardware:** GPIO pin via .data table at `*0x08018748` — actual pin must be
read from live device. **NOT a timer/PWM** — software writes BRR/BSRR registers
with `cpsid i` + `delay_us` loop.

**Gating:** "Sound/Warnings" setting at flag `*0x08004E34` — disables all beeps
when off.

**Beep variants (each is `tone(freq, 100ms)` unless noted):**

| Address | Freq/Dur | Purpose |
|---------|----------|---------|
| 0x0801871C | 200 Hz, 10ms | **Key beep** (every accepted button press) |
| 0x080186CA | 1000 Hz, 1000ms | **Long confirm** (36 call sites — most used) |
| 0x080186D8(n) | n× 900 Hz/100ms | **Multi-beep cascade** (boot, tapping) |
| 0x080186FA(n) | n× 900 Hz/50ms | **Error chirp** |
| 0x08018728 | 227→191→170 Hz | **OK chord** (menu confirm) |
| 0x08018738 | 227→255→255 Hz | **Error chime** (NACK) |
| 0x0801866A-0x080186BE | 255-127 Hz, 100ms | tonal variants for melodies |

**Triggers found:**
- Key press → 0x0801871C (gated by sound-enable flag)
- Menu confirm → OK chord (0x08018728)
- Boot/tap completion → multi-beep cascade
- Tapping cycle end → long confirm (0x080186CA)
- **NO depth-reached beep, NO motor-fault beep** in OEM firmware

For our firmware: we have more flexibility — could add depth-reached and
fault beeps that the OEM lacked.

### F0 Fault Code → LCD Message Table (complete, from disasm 0x08006b90)

Dispatch function at 0x08006b90. Three language blocks (DE/FR/EN) gated on RAM
byte 0x2000002c. Layout: `beq` for code 5, `tbb` table for 0-4, `cmp/beq` chain
for 13, 14, 50, 55, 56. All else → generic WARNING screen.

| F0 | English Message | Action |
|----|----------------|--------|
| 0 | "Unexpected Fault" | display only |
| 1 | "SRM not Rotate" / "Check motor connection / Check drill can rotate" | display only |
| 2 | "RPS State Error 0" / "RPS connection loose / Clean position sensor" | display only |
| 3 | "RPS State Error 1" / (same secondary lines as 2) | display only |
| 4 | "Hardware fault" / "MAY require servicing" | display only |
| 5 | "Unexpected Trap" (single line) | display only |
| 13 | "LOW Voltage" (UVL) | display only |
| 14 | "PFC Fault" | display only |
| 15 | (filtered — early-return at 0x08006b76 if comm-flag clear) | no display = idle |
| 50 | "Invert. OverHeat" / "Let motor cool down / Reduce power limit" | display only |
| 55 | "EEPROM Reset" / "Possible PFC Fault" | **display + MCB action** — sends 1-byte MCB cmd via 0x080181f4(2,0) (likely EEPROM/PFC clear) |
| 56 | "EEPROM FAULT" | display only |
| 6-12, 16-49, 51-54, 57-99 | "Control Board Issue / MAY require servicing" (generic) | display only |
| 100+ | Full-screen "Unexpected Trap" via 0x801a038 | display only |

**Key observations:**
- Header banner "WARNING!" / "ATTENTION!" / "WARNUNG!" printed once before dispatch
- Only F0=55 has side effects beyond LCD — sends an MCB command (EEPROM clear)
- **No code in this routine issues motor stop or SYSRESETREQ** — caller is
  expected to have already stopped the motor before showing fault screen
- F0=15 ("no active fault") is filtered upstream — never reaches the dispatch
- LCD helpers: 0x8019ec8 (string write at row), 0x8019ce2 (clear WARNING screen)

### Fault Codes (F0 query response)
**CORRECTED 2026-03-03 — decoded from disassembly string tables (firmware_r2p06k_cg.asm)**

| Code | Meaning | Notes |
|------|---------|-------|
| 0 | Unexpected Fault / Control Board Issue | Unexpected/unknown internal fault |
| 1 | SRM not Rotate / Check motor connection / Check drill can rotate | **SRM rotor not spinning** |
| 2 | RPS State Error 0 / RPS connection loose | Rotor position sensor 0 error |
| 3 | RPS State Error 1 | Rotor position sensor 1 error |
| 4 | Hardware fault | Hardware fault report |
| 5 | Unexpected error | — |
| 6–12 | *(no message — generic WARNING screen)* | Unclassified by HMI |
| 13 | Low Voltage | **UVL** — triggered at power-down ✓ |
| 14 | PFC Fault | **PFC fault** ⚠ (NOT "Motor Lock") |
| 15 | *(no message — generic WARNING screen)* | **No active fault / MCB idle default** |
| 50 | Inverter Overheated | — |
| 55 | EEPROM data fault | — |
| 56 | EEPROM Error | — |

**F0=15 behaviour:** F0 returns 15 when there is no active fault (MCB idle/default value).
Only shows a specific code (0-14) when GF bit 14 is set (error state).
Power-down UVL: F0=13 during the event, returns to 15 after recovery.

**Motor type confirmed: Switched Reluctance Motor (SRM)**
F0=1 = "SRM not Rotate" explicitly names the motor type.
F0=2,3 = Rotor Position Sensor (RPS) errors — SRM requires shaft position feedback for commutation.
F0=14 = PFC fault — the MCB includes a Power Factor Correction stage on the AC input.

**Error Recovery Sequence** (observed via logic analyzer):
```
[error detected] GF=16929 (ERROR+545)
→ F0?           Query fault code
← F0=13         Under Voltage Low
→ RS=0          Attempt reset
→ JF=1706       Set forward
→ GF?           Check status
← GF=16929      Still in error (retry every ~4s)
```

### Boot Sequence (from original firmware logic analyzer capture 2026-01-25)

```
[0.00s] RS=0 × 3        → Ensure motor stopped
[0.14s] KR? GF? GF?     → First status poll
[0.52s] RS=0            → One more stop
[0.53s] GV?             → Query MCB version
[0.55s] ← GV=GB1.7      → MCB firmware version
[1.61s] ...polling...   → GF? GF? KR? every ~500ms (idle rate)
[7.08s] RS=0            → Pre-init stop
[7.11s] JF=1706         → Set forward direction
[7.23s] SV? → 900       → Read current speed
[7.27s] SV=900          → Confirm speed setting
[7.33s] JF=1706         → Set forward again
[7.45s] S2? → 900       → Read Speed2 parameter
[7.82s] CL? → 100       → Read current limit (100%)
[7.87s] SV? → 900       → Verify speed
```

**Boot defaults:** GV=GB1.7, SV=900, S2=900, CL=100%, KR=0%, GF=32 (STOP)

### Motor Start/Stop Sequence (logic analyzer capture 2026-01-25)

**Start Motor:**
```
→ ST=0              ← START command (not ST=1!)
← KR=100            ← 100% load during spin-up
← GF=34 (RUN)       ← Motor running
← CV=0              ← Not spinning yet (takes ~2s to ramp)
```

**Ramp-up behavior (SV=400 target):**
| Time | KR (Load) | CV (Actual RPM) | Notes |
|------|-----------|-----------------|-------|
| +0.0s | 100% | 0 | Full current at start |
| +0.3s | 21% | - | Current dropping |
| +0.7s | 11% | 246 | Spinning up |
| +1.1s | 11% | 366 | Accelerating |
| +1.8s | 10% | 396 | Near target |
| +2.6s | 11% | 398 | Stable (~99% of target) |

**Speed change while running:**
```
→ SV=600            ← New target speed
← KR=13-15%         ← Load increases during acceleration
← CV ramps          ← 399 → 489 → 515 → 588 → 599 RPM
```

**Stop Motor:**
```
→ RS=0              ← STOP command
← KR=0              ← Load immediately drops
← GF=32 (STOP)      ← Motor stopped
← CV=0              ← Velocity reports 0 IMMEDIATELY (see coast-down below)
```

### Coast-Down RPM is NOT Available (confirmed 2026-05-26)

**The MCB does NOT expose freewheeling/coast-down rotor speed.** The moment RS=0 is
sent, the MCB reports GF=32 (stopped) and CV=0, even though the spindle physically
coasts for several seconds afterward.

**Direct test (motor_test SPINDOWN command):**
- Running at 1500 RPM: CV=1503
- After RS=0: CV=0 immediately, stays 0 for the full 4s polling window
- Same result with PD4 LOW+RS=0 AND RS=0-only (hardware enable is not the variable)

**Original firmware behavior (logic analyzer capture):**
```
[85.79s] RS=0           ← user pressed OFF at 1600 RPM
[85.83s] 0x0052 binary  ← stop variant
[85.86s] RS=0
[86.24s] RS=0           ← triple-stop pattern
[86.26s] GV? → GB1.7    ← version query
         ── ~6.5s of TOTAL SILENCE (spindle physically coasting) ──
[92.81s] RS=0, SV=900…  ← re-init sequence begins
```

The original Teknatool firmware does NOT query CV — or anything — during the
coast-down. It sends RS=0, goes silent for the entire coast, then re-initializes.
Teknatool's engineers knew the data wasn't available and didn't attempt to read it.

**Why:** The RPS (rotor position sensor) is wired to the MCB, not the HMI MCU. The
MCB uses RPS internally for commutation but only exposes *commanded/drive* velocity
via CV. Once RS=0 puts the MCB in stopped state, CV reports 0 regardless of physical
rotor motion. No protocol register exposes the freewheeling speed.

**Firmware implication:** Don't try to display declining RPM after stop — force-zero
the displayed RPM on stop. Show target (setpoint) vs actual (CV) separately during run.

**Post-stop re-sync sequence:**

After motor stops, original firmware re-synchronizes HMI↔MCB state:
```
[+0.0s] RS=0              → Stop command
[+0.4s] RS=0 × 2          → Triple-stop (like boot)
[+0.4s] JF=1706           → Reset to forward direction
[+0.6s] SV? ← 600         → Read current speed from MCB
[+0.6s] SV=600            → Confirm/re-sync speed setting
[+0.8s] JF=1706           → Set forward again
[+0.8s] S2? ← 900         → Verify Speed2 unchanged
[+1.2s] CL? ← 100         → Verify current limit unchanged
```

**Purpose:** MCB is a separate controller with independent state. After motor
operations, HMI re-syncs to catch any changes (voltage sag, thermal protection,
parameter drift) and ensure consistent state before next start. Mirrors boot
initialization sequence.

**Key observations:**
- **ST=0** is START (parameter is always 0)
- **KR=100%** at startup, settles to **10-11%** unloaded
- **KR=13-15%** during acceleration
- **CV** tracks actual RPM, reaches ~99% of SV target
- Ramp time: ~2 seconds from 0 to target
- Poll pattern when running: GF? CV? KR? every 100ms (~54ms actual query time); speed changes use fire-and-forget SV (no response wait)
- Poll pattern when idle: GF? GF? KR? every 500ms
- Logic analyzer verified: MCB responds to explicit queries only (no unsolicited data)

### Spindle Hold Sequence (discovered 2026-01-24)
Powered position lock - applies low current to actively hold spindle position.

**Start Hold:**
```
VR=0, CL=0, VS=0       → Initialize (all off)
V8=264, VG=261         → Set voltage parameters
VR=100, CL=10, VS=1    → Enable hold (full ramp, 10% current, voltage on)
```

**Maintain Hold (repeat periodically):**
```
VR=100, CL=10, VS=1    → Repeat sequence to maintain position
```

**Release Hold:**
```
RS=0                   → Single stop command releases hold
```

**Serial Commands:** `HOLD` (start), `RELEASE` (stop)

**Note**: Commands in bold were discovered via logic analyzer captures (2026-01-22, 2026-01-24).

### JF Jog/Positioning Mode (discovered 2026-01-24 via disassembly)

Found at address 0x801a504 in original firmware. Used by **Rotor Position Test** in service mode
for Hall sensor alignment calibration. Enters a low-torque positioning mode.

**Why it "felt like a normal start"**: Our test command enters JOG mode (JF=3670) and immediately
exits (JF=3669) without sending positioning commands in between. The motor briefly energizes at
low torque then stops - essentially a no-op without actual alignment commands.

**JF Parameters:**
| Value | Hex | Description |
|-------|-----|-------------|
| 1706 | 0x6AA | Continuous FORWARD rotation |
| 1707 | 0x6AB | Continuous REVERSE rotation |
| 3670 | 0xE56 | JOG_START - enter positioning mode |
| 3669 | 0xE55 | JOG_END - exit positioning mode |

**Jog Function Pseudocode:**
```c
void jog_function(void) {        // 0x801a504
    send_command(JF, 0xE56);     // Enter jog mode
    do {
        delay(1);
        flags = query_GF();
    } while (flags & 0x08);      // GF bit 3 = movement in progress
    motor_stop();                // RS=0
    send_command(JF, 0xE55);     // Exit jog mode
}
```

**Chip Breaker Tapping** (captured 2026-01-24): Uses standard RS→JF(1706/1707)→ST sequence, NOT jog commands.

### AC Tapping & Unlock Func (decoded from disassembly 2026-05-26)

**Service Menu structure:**
```
Service Menu (5 items):
  0: Motor Param          → MCB parameter editor
  1: Unlock Func: YES/NO  → Toggles bit 3 of EEPROM[0x48]
  2: Sensor Align          → Rotor position sensor test
  3: AC Tapping            → Sends SX=<value> to MCB
  4: (Height Sensor?)
```

**Unlock Func** toggles bit 3 (mask 0x08) of the HMI EEPROM feature flags at
offset 0x48. When enabled (YES), the normal **Configuration menu gains a 5th item**
(r4 changes from 4→5 items at 0x8012a42). This hidden item is the AC Tapping
configuration screen.

**AC Tapping** sends SX (0x5358) to the MCB with a parameter value (FAQ default=800,
our MCB has SX=0). SX configures the MCB's tapping reversal behavior — likely the
reverse speed/timing for the tap-out cycle.

**JF=3670 jog mode is NOT used for tapping.** Confirmed dead code — zero callers in
all firmware versions, non-functional on GB1.7 MCB (acts as simple start, GF bit 3
never set, SX writes ignored). AC Tapping uses standard JF=1706/1707 direction
reversal with GF bit 2 polling (see below).

### AC Tapping — Complete Protocol (from disassembly 0x8004760 + 0x8009116)

The "Tapping" screen (unlocked via Unlock Func) provides:
- **Power** = CL value (default 5% — very low torque so tap slips, not breaks)
- **Depth** = target depth from depth sensor
- **START TAPPING** button triggers the cycle

**Start sequence (0x8004760):**
```
SV=<speed>        → Set tapping speed (150 RPM from menu)
CL? → save        → Read and save current CL
CL=5              → Drop current limit to 5% (torque-limited)
JF=1706           → Set forward direction
ST=0              → Start motor
```

**Forward/reverse cycle (0x80090f2 / 0x8009116):**
```
Forward:
  JF=1706                → Set forward
  loop: delay(1ms)
        GF = query_GF()
        while (GF & 0x04) → Wait for direction-change to settle
  
Reverse (at depth):
  JF=1707                → Set reverse
  loop: delay(1ms)
        GF = query_GF()
        while (GF & 0x04) → Wait for direction-change to settle
```

**GF bit 2 (0x04) = direction-change-in-progress** — the MCB sets this bit while
switching between forward and reverse commutation, clears it when settled.

**Depth monitoring** (0x8009966): The tapping state machine uses fixed-point depth
calculations to decide when to reverse. Two code paths exist:
- 0x8009918: tapping cycle with mode selection (mode 0/1/2 → different state counts)
- 0x800a18a: depth-sensor-triggered reversal

**No jog mode, no SX parameter** used in the actual tapping implementation.

### Reverse Direction Handling (decoded 2026-05-26)

**Where reverse is engaged (verified empirically 2026-05-26):**
- Accessed via the **"Advance Modes" menu** (menu index 7 in main menu)
- Menu item "Direction: FORWARD/REVERSE" — handler at 0x800d3fe
- **FWD/REV IS a bindable F-key function** (disasm agent was wrong about this) —
  available in the F-key Menu Shortcuts function list
- **F-key direction change works only when motor STOPPED** — same guard as the
  menu toggle. While running, pressing the F-key bound to FWD/REV just bumps
  the RPM briefly (re-sends ST/SV without changing direction).
- **Not persisted across power cycles** — boot always starts in FORWARD ✓

**Direction toggle sequence (from stopped state):**
1. Read live direction from cache (reg=4 bit 0x100, mirror of MCB GF bit 2)
2. Call 0x8004696 with r0=0 (FWD) or r0=1 (REV):
   - FWD → JF=1706 + clear EEPROM reg bit 0x200
   - REV → JF=1707 + set EEPROM reg bit 0x200
3. Read GF, sync bit 2 → reg=4 bit 0x100

Note: This does NOT send RS=0 or ST=0 — the motor is already stopped here.

**Reverse start sequence (from 0x8004760):**
```
SV=<speed>              → set speed
delay 10ms
read actual speed (CL? readback)
0x8004696(1)            → JF=1707
0x80045f0:
  poll GF & 0x4000      → wait for not-error
  ST=0                  → start
  poll GF & 0x02        → wait for running bit set
```

**Mid-run reversal (tapping/chip-breaker path at 0x80090c6):**
```
delay 5ms
poll GF & 0x02 clear   → wait for commutation idle
JF=1706 or 1707        → DIRECT direction change (no RS/ST cycle)
poll GF & 0x04         → wait for direction-change-complete
```
**Commutation-only reversal — no full stop/start cycle.** The MCB handles
the reversal internally via its commutation logic.

**Direction persistence:** **NOT persisted across power cycles.** The direction
cache bit is repopulated from a live GF query at boot (`0x801a484`). MCB power-on
default is GF=32 (FWD). Even though EEPROM 0x3A bit 1 was earlier labeled as
"direction setting", **no code path loads it into the direction cache** at boot.
Every power cycle starts in FORWARD.

**No interlocks:** Same speed range for FWD and REV, no calibration gate, no
power-level gate.

**GF bit meanings (refined):**
| Bit | Mask | Meaning |
|-----|------|---------|
| 1 | 0x02 | Motor running |
| 2 | 0x04 | Direction-change in progress (set during commutation reversal) |
| 14 | 0x4000 | Error state (check F0) |

The 0x04 bit is the key "direction-change settling" indicator used by all
direction-change paths.

**GF observed values on GB1.7 (verified 419 samples + full sigrok session):**
| GF Value | State |
|----------|-------|
| 32 (0x20) | Stopped (any direction) |
| 34 (0x22) | Running (any direction) |
| 16929+ | Error (bit 14 set; F0 has details) |

**Important correction:** Earlier docs listed GF=436 (stopped rev) and GF=438
(running rev). These were NOT observed in any capture on GB1.7. GF appears to
carry no direction info — direction must be read from GR bit 2 (see Reverse
Direction section).

**GF response is ALWAYS single-field — verified rigorously.** No commas, no
multi-field comma-separated format like "flags,speed,load,vibration,temp".
Tested 419 samples across stopped/ramp/steady/reverse/stop states, plus a
full sigrok of original firmware running through direction toggles. Every
response was the standard `[STX]['1']['G']['F']<digits>[ETX][XOR]` format.

### Response framing, and the bug it hid (corrected 2026-08-30)

A reply is `[ACK]? [STX] [unit] [cmd_H] [cmd_L] <ascii digits> [ETX] [XOR]`.
A REQUEST is different — `SOH + "0011" + '1' + cmd + ENQ` — and v0.1.0 shipped
`protocol_validate_response()` checking the REQUEST framing against responses.
It therefore rejected every genuine reply and **`motor_read_param()` never
returned a value**: `MCBPARAMS` always said "MCB not responding", power-level
readback always reported a mismatch, `motor_factory_reset()` always reported
failure even when it worked, and every console read printed -1.

Two further defects sat behind it. The motor task owns USART3 but never took
`g_motor_mutex`, so the mutex excluded every task except the one that mattered;
and `motor_read_param()` slept 2 ms between polls on a bare RXNE read with no
buffering, which overruns after one byte at 9600 baud. All three are fixed;
`T0`, `UD` and all nine `MREAD` registers read correctly on hardware now.

`protocol_validate_response()` also SCANS for the frame whose command echo
matches, because the MCB emits CV updates unsolicited and one can land in the
read window ahead of the reply being waited for.

**`GR` answers, intermittently.** It returned -1 on every attempt before these
fixes, which this document previously recorded as the MCB not answering. It
does answer: `ALIGN A -> GR=4 RPS:..C`, though `ALIGN B` still yields -1 on some
attempts. `cmd_align` uses a fixed 100 ms delay with no retry, which is the
obvious next suspect.

**Implication for our firmware:** The multi-field GF parser at
`parse_gf_response()` in `src/motor.c` (cases 1-4 for speed/load/vibration/temp)
is **DEAD CODE** — those fields are never populated by GF responses.
- `motor_status.vibration` is therefore always 0 — `motor_get_vibration()`
  always returns 0. Affects jam detection in `jam.c:248`.
- `motor_status.load_percent` from GF parser never fires — must use KR query
- `motor_status.speed_rpm` from GF parser never fires — must use CV query
- Recommendation: delete cases 1-4 of `parse_gf_response`, source those fields
  from their dedicated query handlers (CV for speed, KR for load, TH/T0 for
  temp, vibration is unobservable so remove that feature)

### Front-Panel Buttons & F-Key Mappings (decoded 2026-05-26)

**Verified GPIO pin assignments (empirical 2026-05-26 via BTNS command):**

| Button | Port | Pin | Active |
|--------|------|-----|--------|
| F1 | GPIOC | **PC10** | LOW (pressed) |
| F2 | GPIOC | **PC11** | LOW |
| F3 | GPIOC | **PC12** | LOW |
| F4 | GPIOD | **PD2** | LOW |
| ON | GPIOA | **PA15** | LOW |
| MENU | GPIOB | **PB4** | LOW |
| ZERO/Confirm | GPIOB | **PB3** | LOW |
| OFF | — | **NRST** | Hardware reset (not a GPIO input) |
| Chuck Guard | GPIOC | **PC2** | HIGH = guard OPEN |
| E-Stop | GPIOC | **PC0** | polarity TBD |

Main firmware config.h F-key pins are **correct as-is** — earlier disasm agent
claim of PD10/11/12 was wrong (misread port index from literal pool).

Debounce: 20 reads × 2ms (~40ms total).
GPIO read helper at 0x801c23c.

**OFF button is wired to NRST** — pressing OFF triggers a soft hardware reset,
not a GPIO input. This explains the "MCB init: RS x3 + GV query..." sequence
seen when OFF is pressed in the original firmware.

**Configurable F-Key functions (NOT just speed presets):**
The 4 bytes at EEPROM **0x50/0x52/0x54/0x56 are per-F-key function assignments**.
Editable via "Menu Shortcuts" page (handler 0x8011eaa). The function list spans
**3 pages** (LCD shows "1/3" pagination). Verified function codes (incomplete):

| Code | Function | Notes |
|------|----------|-------|
| 0 | -Do Not Use- | (page 1) |
| 1 | Fav. Speed | toggles between 2 favorites (page 1) |
| 2 | +/- Run Speed | nudge (page 1) |
| 3 | Change UNITS | (page 2, guessed) |
| 4 | Menu Shortcut | (page 2, guessed) |
| 5 | Lock Drill | (page 2, guessed) |
| 13 | "Links" / Direction toggle | empirical: shows "LI" label, toggles FWD/REV when motor stopped |
| 26 | Brake / "Bremse" | empirical: shows "BREMS" label |

Codes 6-12, 14-25, 27+ exist but weren't enumerated. Total function count
unknown — disasm agent's "0-5" list was only **page 1** of the menu.

EEPROM 0x98 = **packed F-key function bitfield** (default 0x29 = 0b00101001,
2 bits per key × 4 keys).

**Fav.Speed mode pairing (each F-key toggles between two presets):**
| F-key | Preset A | Preset B |
|-------|----------|----------|
| F1 | EEPROM 0x64 (#1) | EEPROM 0x74 (#5) |
| F2 | EEPROM 0x68 (#2) | EEPROM 0x78 (#6) |
| F3 | EEPROM 0x6C (#3) | EEPROM 0x7C (#7) |
| F4 | EEPROM 0x70 (#4) | EEPROM 0x80 (#8) |

So the 8 speed presets aren't 8 independent slots — they're 4 pairs, one per F-key.

**Preset editing (digit-entry mode at 0x8007a04):**
- F1 = ±1000 (thousands)
- F2 = ±100 (hundreds)
- F3 = ±10 (tens)
- F4 = ±1 (ones)
- O = cycle digit / +/- toggle
- M = save and exit
- On save: `eeprom_write_word(0x64 + (slot-1)*4, rpm)`

**F-key action when pressed (running or stopped):**
- Resolves to target RPM (from Fav.Speed pair or +/- nudge)
- Single `SV=<rpm>` write via 0x8004702
- No convergence loop (that's service-menu only)
- Display shows the new SV (target), not CV (actual)

### Profile (FD) and Soft-Start — All on the MCB Side

**FD encoding (verified empirically 2026-05-26 via FDTEST at 1500 RPM):**
| FD | Profile | Ramp time to 90% |
|----|---------|------------------|
| 0 | **Normal** | 1233 ms (factory default) |
| 1 | **Soft** | 2382 ms (slowest) |
| 2 | **Hard** | 390 ms (fastest) |

The disasm-agent claim of `0=Hard, 1=Normal, 2=Soft` was wrong. Empirical
test runs FD through 0/1/2 and measures CV ramp from STOP to 90% of target.
Result matches original Voyager menu intuition: Normal is default, Hard is
the aggressive setting (FD=2).

**HMI does NOT ramp SV.** The motor-start sequence (0x8004760) does a single
`SV=<target>` write — no client-side ramping. All ramp behavior lives in the
MCB and is selected by FD + governed by SR.

**SR (speed ramp) is in 0.01-second ticks.**
- SR=1000 → 10 seconds full-scale ramp
- Service menu range: 10-500 (0.1s to 5.0s display), stored ×10 in MCB
- Per-tick MCB ramp rate = max_speed / SR ticks

**S0-S9 (speed profiles in REGSCAN) are MCB factory ramp tables.**
- Never read or written by the HMI at runtime (zero non-wrapper callers)
- The values S0=2500, S7=750, S8=2000 etc. are MCB-internal ramp curve points
  for Hard/Normal/Soft, NOT user-editable speed presets
- The user's speed presets at HMI EEPROM 0x64-0x80 are completely separate

**Live speed change from encoder (0x800ac7e):**
- Calls `set_speed_with_check` (0x8004702) → `set_speed_clamped` (0x80046c2)
- Single `SV=<new>` write, no HMI ramping
- After write, reads SV back via 0x801a69e and displays the readback
- **Display shows commanded SV (target), not CV (actual)** — no smoothing

**For our firmware:**
- Don't implement HMI-side ramping — write SV directly, MCB does the rest
- FD selects MCB ramp profile (we already have these defined)
- Display the target speed (SV readback) for user-perceived "instant" change
- Optionally show CV as "actual" secondary value
- SR controls global ramp duration; FD selects ramp shape

### Safety Inputs: Chuck Guard & E-Stop (decoded 2026-05-26)

The "_cg" suffix on all firmware variants refers to **Chuck Guard** — a hardware
safety interlock with a corresponding HMI modal screen.

**GPIO pins (from disasm):**
| Signal | Pin | EXTI | Polarity | Notes |
|--------|-----|------|----------|-------|
| **Chuck Guard** | **PC2** | EXTI2 | Active HIGH (1=OPEN, NC wired to GND) | Modal at 0x0801d350 |
| **Emergency Stop** | **PC0** | EXTI0 | Active HIGH (1=PRESSED, NC wired to GND) | Modal at 0x0801d1f4 |
| Front-panel buttons | PD2, PA15, PC10-12, PC15, PB3-4 | — | Active LOW | Boot self-test checks stuck buttons |

**Both safety inputs use failsafe NC wiring** (verified empirically 2026-05-26):
- Safe state: NC contact closed → pin tied to GND → reads LOW
- Triggered: contact opens → pin floats → internal pull-up brings it HIGH
- **A broken wire triggers the alarm just like a button press** — correct failsafe design

**Both safety inputs use the same architecture:**
1. EXTI interrupt fires on input rising edge.
2. ISR (chuck guard at 0x0800628e, e-stop at 0x08006252) confirms pending bit.
3. **Debounces** — requires 10 consecutive reads with 1ms gaps.
4. `cpsid i` — masks ALL interrupts.
5. Calls modal screen — displays warning in current language.
6. Modal loops reading the input every 50ms.
7. When input clears (10 consecutive reads back to 0) → `NVIC_SystemReset()`
   (writes 0x05FA0004 to SCB->AIRCR = full MCU reboot).

**NO software gating of motor start** — there is no `HAL_GPIO_ReadPin(GPIOC, PC2)`
check anywhere in the motor-start paths (0x080045f0, 0x08004696, 0x08004760).
The motor is started unconditionally — the safety interlock is entirely **hardware**:
the chuck-guard switch is presumably in series with the MCB safety chain or the
PD4 motor-enable line. If the guard is open, the EXTI is already pending and the
ISR fires before the motor can spin up; the HMI just shows the modal.

**No EEPROM bypass** — the chuck guard cannot be disabled from any service menu.
The earlier "Unlock Func" toggle is about a different password feature, NOT this.

**Mid-run behavior (guard opens while motor running):**
- HMI sees EXTI2 fire → modal "Chuck Guard / Opened / Close to continue"
- HMI does NOT send a stop command to the MCB (assumes hardware already cut motor)
- User must physically close the guard
- 10 consecutive closed reads → MCU reboot
- MCB sees its power cut (or safety signal asserted) → motor stops on its own

**Strings (with German/French/English variants):**
- "Chuck Guard" / "Opened" / "Close to continue"
- "Emergency Stop" / "Pressed" / "Twist to release"

**For our firmware** — we need:
1. EXTI2 ISR on PC2 with debounce and modal display
2. EXTI0 ISR on PC0 same
3. Don't bother adding software motor-stop on guard open — hardware handles it
4. After modal clears, reboot (the original behavior) — OR we could be smarter
   and just resume since our firmware can re-establish state

### Depth/Height Sensor — LOCAL ADC, NOT the MCB (verified 2026-05-26)

**CORRECTION (the earlier disasm-agent claim of MCB-side depth was WRONG):**
The quill depth is read from a **local linear potentiometer** on:
- **PC1 = ADC1 Channel 11** (12-bit, 0-4095)
- Verified empirically: values smoothly track 40-2244 across quill travel

Main firmware config.h has this correct:
```c
#define DEPTH_ADC           ADC1
#define DEPTH_ADC_CHANNEL   11
#define DEPTH_ADC_PORT      GPIOC
#define DEPTH_ADC_PIN       GPIO_PIN_1
#define DEPTH_COUNTS_PER_MM 41
```

The disasm agent missed this because they searched only for `HAL_TIM_Encoder_Init`
and MCB UART queries — they didn't grep for ADC1 SQR/SMPR register writes.

**Calibration (from disasm linear regression routine at 0x8005320):**

The "Height Sensor" service-menu calibration uses **6 sample points** at
known positions (0, 20, 40, 60, 80, 100 mm) and computes ordinary least-squares:
```
mean_x = Σ(raw[i]) / 6
mean_y = Σ(setpoint[i]) / 6
Sxy = Σ (raw-mean_x)(setpt-mean_y)
Sxx = Σ (raw-mean_x)²
slope     = Sxy / Sxx           // float divide via 0x801b51c
intercept = mean_y - slope*mean_x
R² = correlation^2              // must be ≥ 0.98 (0x3F800000 threshold)
```

If R² < 0.98 calibration is rejected.

**EEPROM storage (revised from earlier 3-value theory):**
The "three calibration values" are actually **TWO 32-bit packed fixed-point** values:
- EEPROM `0x16` (4 bytes, `0xC1A9 0x00B1`) = **slope** (32-bit fixed-point)
- EEPROM `0x23` (4 bytes, `0x3F12 0x4E12`) = **intercept** (32-bit fixed-point)
- `0x1E` and `0x1A`/`0x1C` are **redundant copies** for fault detection

The earlier interpretation as 3 separate 16-bit values (A, B, C) was wrong.

**Runtime depth calculation:**
```c
raw = ADC1_IN11_read();                 // PC1 potentiometer, 0-4095
height_mm_x1000 = raw * slope + intercept;   // fixed-point arithmetic
display = sprintf("%d.%03d", h/1000, h%1000);    // "12.345"
```
Display is fixed-point × 1000 (3 decimal places).

**Depth-stop logic (0x8007816):**
- Single 16-bit value at EEPROM `0x28-0x29`
- Continuously compared in display loop against current depth
- On match: motor-stop command sent
- **No depth-preset array** in OEM firmware — only one stop value. The F3-long-press
  preset cycling in Nova Voyager firmware is a Nova-specific addition.

**Redundant copy mismatch handling (0x8017c96):**
```c
if (read32(0x16) != read32(0x1E)) {
    settings_fault();        // → "EEPROM FAULT" banner
    zero(0x1E); zero(0x23);  // wipe redundant copies
}
```
Mismatch wipes the field and forces recalibration.

**Depth source RESOLVED (2026-05-26):** Not an MCB register at all. Empirical ADC
poll on PC1 showed values smoothly tracking 40-2244 as quill moves through travel.
HT register stays constant at 75 (it really is heatsink temp). The MCB exposes no
depth/quill register because the quill sensor is wired directly to the HMI's ADC.

### MCB Polling Cadence — NO Heartbeat (confirmed 2026-05-26 via disasm)

**The MCB is stateless w.r.t. polling cadence.** The original firmware NEVER sends
any command "just to keep MCB alive." Every transaction is event/flag-driven from
the UI or dispatcher. The HMI can poll at any rate; missing a "heartbeat" will not
fault the MCB.

**The "7-second boot loop" myth — debunked:**
What looked like a fixed 7s wait at boot is actually the **periodic-event dispatcher**
running its normal flag-driven loop. The `7` is a **rolling-average sample count**
for KR (function around 0x800438a):
- `[0x8004554]` = sample counter, ++ per dispatch tick
- `[0x8004558]` = accumulator, += current sample
- When counter reaches 7: smoothed = acc/7, reset both
- The "7 seconds" we measured was just dispatcher cycles, not a designed wait

**The 6.5s silence after OFF — explained:**
NOT a designed quiet period — it's the **retry budget of failed queries**. The
query function at `0x801b0b6` retries up to **15 times**; each inner send/receive
loop also retries 15 times with `delay(5)` ≈ 75ms. So **one failed query** consumes
~15 × 75ms ≈ **1.1s**, and a few failed KR/GF queries stack to ~6.5s before the
firmware gives up. There's no timer-driven probe — silence ends only when the next
UI/state-machine event schedules a query.

**Steady-state polling:** Event-driven, no periodic MCB watchdog. The fault
dispatcher (`0x801cae0` system reset) is hit on RAM/CRC/PFC errors — never on
"no KR response in N seconds." Failed queries just return 0 and set an error flag.

**"SV=300 ×15" in captures — corrected explanation:**
This is the **write-retry loop inside `motor_send_command` at 0x801b110**.
If the MCB doesn't ACK, the function retries up to **15 times** (check at
0x801b228: `cmp r9, #15`). The captures show every retry attempt because the
MCB occasionally drops bytes at startup. It is NOT a deliberate convergence
loop or refresh — just retry-on-NACK.

**The convergence loop at 0x801a5c6** (VR=0, CL, LC, VS=1, GR stable check)
exists but is **service-menu-only** — called from sensor/PID calibration paths
(0x800e802, 0x801c346, etc.), never from runtime motor start.

**Don't add periodic CL/SV refreshes** in our firmware — the MCB latches values,
and the only retry needed is on a failed UART transaction.

**Query function retry budget (important for our firmware):**
- Per-query retries: 15 (at `0x801b0b6`)
- Per-byte timeout: ~5ms in the inner receive
- Max worst-case time per failed query: ~1.1s
- Our `motor_read_param` and `task_motor` should use shorter timeouts (we don't
  need 15× retries — 2-3 with shorter inter-attempt delay is faster fault detection)

### KR (Load %) Usage in Original Firmware — Jam/Stall Detection

KR is queried **exactly once** in the dispatcher state machine and the value is
cached at RAM 0x2000002a. All other consumers (LCD, tapping, etc.) read the cached
value. Polling rate: once per dispatcher cycle (state-machine bit 16 path).

**Three independent monitors process the cached KR/CV:**

#### 1. Stall watchdog (RPM-based, NOT load-based) — at 0x80042d4
```
if (CV < 25 RPM)         → stall_counter++
if (stall_counter > 40)  → SYSRESETREQ (hard MCU reset)
```
Triggers when motor is commanded but tach reports near-zero for 40 consecutive
ticks — interpreted as MCB or encoder failure, recovers via reboot.

#### 2. Low-load / no-load detector — function at 0x800a854 (Branch A)
```
if low_load_enabled[0x200000e6]
   AND KR < user_threshold[0x200000e7]      // e.g. "below 5%"
   AND CV < 25 RPM:                          // tach also low
      no_load_counter++
   else:
      no_load_counter = 0

if no_load_counter > 18:
   stop motor (graceful, via 0x8004808)
   set fault = 2
```
Detects belt break, tool detached, or other "motor spinning but no real load"
conditions. 18-sample debounce.

#### 3. Jam detector — function at 0x800a854 (Branch B)
```
if jam_enabled[0x200000e4]
   AND prev_KR > 0
   AND KR > 0:
      delta = KR - prev_KR
      if delta > user_step_threshold[0x200000e5]:
         set jam_fault_bit
         stop motor (graceful)
         set fault = 2
prev_KR = KR                                  // always update
```
**Step-change detection** — a sudden jump in KR (drill bit catches) triggers stop.
NOT an absolute threshold but a **delta** between consecutive samples. The threshold
is user-configurable via service menu.

**Key insight:** The original firmware doesn't use an absolute "load > X%" jam
threshold. It looks for a **sudden step UP** in KR (delta) — this catches the
moment the drill bites into harder material, before the load reaches dangerous
levels. The 5% to 30% transition is more dangerous than steady 30%.

**User-configurable jam parameters:**
| RAM | Default? | Meaning |
|-----|----------|---------|
| 0x200000e4 | enable bit | High-load (jam) detector enable |
| 0x200000e5 | step % | KR delta threshold (e.g. 20 = trip if KR jumps +20%) |
| 0x200000e6 | enable bit | Low-load detector enable |
| 0x200000e7 | abs % | Absolute low-load threshold |
| 0x200000e8 | counter | Low-load consecutive samples |
| 0x200000ea | last KR | Previous KR value for delta calc |

**KR is queried ONCE per dispatcher cycle, cached, then consumed by:**
- LCD display (via 0x801a198 itoa printer)
- Jam/stall monitors (both branches of 0x800a854)
- Tapping handler (reads cache, doesn't re-query)
- Any other UI/state code (cache only)

No KR queries in the tapping cycle code — the load slip detection in tapping uses
the same cached value, not a fresh query.

### MCB Tuning Registers — Service Menu Only (SA, NC, BN, BF, UW)

These registers are MCB internal motor tuning parameters. The HMI firmware
**never reads or uses them at runtime** — they exist only in the service menu
parameter editor. Each has exactly 3 code references: query wrapper, command
wrapper, SE commit wrapper. Zero callers from motor control, tapping, or depth code.

| Reg | Code | Our Value | FAQ Default | MCB Function |
|-----|------|-----------|-------------|-------------|
| SA | 0x5341 | 0 | 85 | **AdvMax** — commutation advance angle limit |
| NC | 0x4E43 | 0 | 1000 | **SpdAdvMax** — speed-dependent advance max |
| BN | 0x424E | 0 | 380V | **VdRefON** — DC bus voltage reference ON threshold |
| BF | 0x4246 | 0 | 360V | **VdRefOFF** — DC bus voltage reference OFF threshold |
| UW | 0x5557 | 0 | 300V | **VdLowLim** — DC bus voltage low limit |

**SA/NC (advance angle):** Control how early the MCB fires the next SRM phase
relative to rotor position. Higher values = more aggressive commutation for higher
speeds. With 0, the MCB uses built-in advance tables optimized for GB1.7.

**BN/BF (voltage reference):** DC bus voltage regulation thresholds for PFC (Power
Factor Correction). BN = voltage to turn PFC on, BF = voltage to turn PFC off.
With 0, the MCB uses internal defaults.

**UW (voltage low limit):** Minimum DC bus voltage before MCB shuts down for
under-voltage protection. With 0, the MCB uses its internal UV thresholds
(UV=200V run, UL=100V absolute from REGSCAN).

All at 0 on GB1.7 = "use firmware-internal defaults". The FAQ values (85, 1000,
380, 360, 300) were explicit overrides for an older MCB firmware version.

**Feature flags at HMI EEPROM offset 0x48:**
| Bit | Mask | Purpose |
|-----|------|---------|
| 1 | 0x02 | Direction setting (FWD/REV) |
| 3 | 0x08 | **Unlock Func** — reveals AC Tapping in config menu |
| 5 | 0x20 | Startup feature enable |
| 6 | 0x40 | Configuration flag |
| 7 | 0x80 | Display config flag |
| 8 | 0x100 | Display config flag |
| 9 | 0x200 | Configuration flag |

**Note:** These flags are stored in the HMI's AT24C02 EEPROM (offset 0x48), NOT
in the MCB. The EEPROM is accessible and WRITABLE via **PC4(SCL)/PC5(SDA)** — the
original firmware's I2C bus. The PB6/PB14/PB15 path found earlier is write-protected
(WP hardwired), but the PC4/PC5 path has no WP issue.

### HMI EEPROM — Correct I2C Bus (discovered 2026-05-26)

**Two SEPARATE AT24C02 chips on the HMI board:**
| Chip | SCL | SDA | Enable | Writable | Content | Used by |
|------|-----|-----|--------|----------|---------|---------|
| PB chip | PB6 | PB14 | PB15 | **NO** (WP=VCC) | All 0x00 | Unused |
| **PC chip** | **PC4** | **PC5** | none | **YES** | Config data | Original FW |

Both respond at I2C address 0x50 but are on different buses with completely different
content — confirmed side-by-side read. The HMI board is a shared platform across
Teknatool products: **PC chip = drill press** (Voyager, "DRIL5.20D_DP"),
**PB chip = likely for lathes** (Nova 1624 etc., zeroed/unused on our drill press).

The original Teknatool firmware uses **PC4/PC5** for all EEPROM access (confirmed
from disassembly: I2C bit-bang at 0x801842e uses GPIOC 0x40011000). Our firmware's
eeprom.c has been updated to use PC4/PC5 bit-bang I2C.

**EEPROM content (read via PC4/PC5):**
```
00: FF D3 7C FF 44 52 49 4C 35 2E 32 30 44 5F 44 50  |..|.DRIL5.20D_DP|
10: FF FF FF FF FF FF C1 A9 B1 00 3F 12 4E 12 C1 A9  |..........?.N...|
20: FF FF FF 3F 12 4E 12 FF 00 00 FF FF FF FF FF FF  |...?.N..........|
30: 00 64 00 FA FF FF FF FF FF FF 02 FF 00 FF FF FF  |.d..............|
40: 00 32 FF FF 0B B8 FF FF EE A0 FF FF FF FF FF FF  |.2..............|
50: 01 FF 02 FF 03 FF 04 FF FF FF 00 04 FF FF FF FF  |................|
60: 19 FF 00 FF 00 FA FF FF 03 84 FF FF 06 40 FF FF  |.............@..|
70: 0B B8 FF FF 01 F4 FF FF 04 B0 FF FF 07 D0 FF FF  |................|
80: 09 C4 FF FF 01 FF FF FF FF FF FF FF FF FF FF FF  |................|
90: FF FF FF FF FF FE FF FF 29 FF FF FF FF FF FF FF  |........).......|
```

**Key fields:**
- 0x04-0x0F: "DRIL5.20D_DP" — model identifier (Drill Press, firmware 5.20D)
- 0x31: 0x64 = 100 (default CL?)
- 0x3A: 0x02 = direction/config
- 0x41: 0x32 = 50
- 0x44: 0x0BB8 = 3000
- 0x48: **0xEE = feature flags** (Unlock=YES, bits 1-3,5-7 set)

**Our firmware must use PC4/PC5 for EEPROM**, not PB6/PB14.

### Sensor Alignment / Rotor Position Test (from Teknatool FAQ, confirmed 2026-05-26)

Official calibration procedure from Teknatool FAQ "Rotor Position Test on the NOVA Voyager
DVR Drill Press" (January 2017). Uses jog mode to energize individual motor phases and
verify rotor position sensor (RPS) alignment.

**MCB registers involved (corrected from disassembly — PW/PH are NOT used):**
| Screen label | MCB Register | Normal | During test | Notes |
|-------------|-------------|--------|-------------|-------|
| PulseW | **PU** (0x5055) | 1000 | 100-400 | Pulse max — NOT PW! |
| CurLim | **CL** (0x434C) | 100 | 10-20 | Current limit |
| Phase | **FD** (0x4644) | 0 | 0/1/2 | Profile/phase select — NOT PH! |
| RPS cond | **GR** (0x4752) | 14 | 1-6 | Sensor bitmask — NOT RP or GF! |

**IMPORTANT:** Disassembly confirms the original firmware NEVER sends PW (0x5057),
PH (0x5048), or RP (0x5250). Logic analyzer capture (2026-05-26) reveals the actual
commands used — completely different from the FAQ's screen labels.

**Actual protocol (captured from original firmware R2P06K via logic analyzer,
verified with motor_test ALIGN command 2026-05-26):**

Screen label → actual MCB register:
- "PulseW" → **VR** (Voltage Ramp, 0x5652) — user adjusts 0→20+ by encoder
- "CurLim" → **CL** (Current Limit, 0x434C) — user adjusts by encoder (no SE commit)
- "Phase" → **VS** (Voltage Set, 0x5653) — 0=A, 1=B, 2=C
- "RPS cond" → **GR** (0x4752) — sensor bitmask

**GR = RPS sensor bitmask (confirmed via logic analyzer + ALIGN test):**
| GR | Binary | Sensors | FAQ notation |
|----|--------|---------|-------------|
| 2 | 010 | B | .B. |
| 3 | 011 | AB | AB. |
| 4 | 100 | C | ..C |
| 5 | 101 | AC | A.C |
| 6 | 110 | BC | .BC |
| 10 | 1010 | ? | idle/transition |
| 11 | 1011 | ? | magnetized |
| 13 | 1101 | ? | stopped baseline |

**Complete alignment procedure (logic analyzer capture + ALIGN test verification):**

Entry — read baseline:
```
VR? → 0        CL? → 100      VS? → 0        GR? → 13
```

Setup — configure voltage hold subsystem:
```
V8=264          → voltage param
VG=261          → voltage gain
VS=<phase>      → phase select (0=A, 1=B, 2=C)
CL=20           → current limit (low power for test)
VR=20           → voltage ramp (magnetize motor — hums)
```

Continuous GR polling (~30ms) — user turns spindle, watches sensors.

Phase switch (A/B/C):
```
VR=0  CL=0  VS=0           → turn off everything
V8=264  VG=261              → reconfigure
VS=<new_phase>              → select new phase
CL=20                       → re-apply current
VR=20                       → re-apply voltage (hum resumes)
GR? GR? GR?...              → rapid sensor polling
```

Exit (press OFF or Q):
```
VR=0  CL=0  VS=0           → turn off
CL=100  VS=0               → restore defaults
(no SE commits — all test values are RAM-only)
```

**Verified sensor alignment results (2026-05-26, motor_test ALIGN command):**

| Phase | VS | GR dominant | Sensors | FAQ expected |
|-------|-----|-------------|---------|-------------|
| A | 0 | 2 / 3 | .B. / AB. | B or AB ✓ |
| B | 1 | 2 / 6 | .B. / .BC | C or BC ✓ |
| C | 2 | 4 / 6 | ..C / .BC | A or AC * |

*Phase C shows C/.BC instead of FAQ's A/A.C — likely different winding order
between our motor and the FAQ's reference unit. All three phases produce distinct,
consistent patterns confirming correct sensor alignment.

**Expected RPS condition for correct alignment:**

| Phase (FD) | Energized | RPS should show |
|------------|-----------|-----------------|
| 0 = A | Phase A | B or AB (sensors B active) |
| 1 = B | Phase B | C or BC (sensors C active) |
| 2 = C | Phase C | A or AC (sensors A active) |

If RPS doesn't match: motor phases may be miswired or sensors misaligned.

**motor_test command:** `ALIGN` — interactive sensor verification.
- Sets V8=264, VG=261, VS=0 (phase A), CL=20, VR=20
- Polls GR (sensor bitmask) and EV (rotor position) continuously
- Press A/B/C to switch phases (changes VS=0/1/2)
- Press Q to exit (restores VR=0, CL=100, VS=0)

**CAUTION:** VR energizes the motor coils (audible hum). CL=40 is strong,
CL=20 is gentle. Values persist in MCB RAM until power cycle. Always exit
cleanly (Q) or restore with `SET VR 0` + `SET CL 100` if interrupted.

**Default motor parameters (from Teknatool FAQ):**

| Parameter | Default | Our REGSCAN | MCB Register |
|-----------|---------|-------------|--------------|
| Profile | Normal | FD=0 | FD |
| Kprop/Kint | Varies | SP=1000, SI=500 | SP, SI |
| V kprop/V kint | 2000/9000 | **VP=0, VI=0** | VP, VI |
| Vd DC Bus | 360V | UD=358 | UD |
| T Threshold | 60°C | HT=75 | HT |

**VP/VI discrepancy:** FAQ lists V kprop=2000, V kint=9000 but our MCB reports VP=0,
VI=0. These may be drill-press-specific defaults, or our MCB firmware (GB1.7) uses
different defaults than the FAQ's target version. The voltage PID (VP/VI) is only used
during spindle hold mode (VS=1), so 0 when hold is inactive may be normal.

**motor_test command:** `ALIGN` runs this test automatically — enters jog mode, sets
PW=40 CL=20, cycles phases A/B/C, reads RP and EV for each, then restores settings.

### Service Mode (from Teknatool FAQ DVD100, January 2017)

**Entry sequence:** Hold Zero/Confirm + press F2 → release both → enter password
**Password:** 3210 (F1×3, F2×2, F3×1) → press ON

**Service Menu structure:**
```
Configuration → Service → Motor Param     (MCB tuning parameters)
                        → Height Sensor   (depth sensor calibration)
                        → Sensor Align    (rotor position test)
```

### Service Mode Motor Parameters vs Our MCB (GB1.7)

Factory defaults from Teknatool FAQ (drill press, January 2017) compared with our
REGSCAN values. **Major discrepancies** — either different motor/product calibration
or our MCB firmware version has different defaults.

#### Service Menu → MCB Register Map (from R2P06K disassembly switch table at 0x8015bc0)

Each menu item: query value + H/L limits, open dial editor, write new value, SE commit.

| Menu Item | MCB Reg (value) | H/L Limits | SE Commit | Our Value | FAQ Default |
|-----------|----------------|------------|-----------|-----------|-------------|
| Profile | FD (0x4644) | — | SE=FD | 0 (Normal) | Normal |
| Kprop | SP (0x5350) | HP / LP | SE=SP | 1000 | Varies |
| Kint | SI (0x5349) | HI / LI | SE=SI | 500 | Varies |
| PulseMax | PU (0x5055) | UH / UL | SE=PU | 1000 | 185 |
| Ir Gain | IU (0x4955) | — | SE=IU | 1000 | 28835 |
| TrqRmp | TR (0x5452) | HT / LT | SE=TR | 75 | 2000ms |

#### Deeper Service Menu (0x8016 region — "Internal/Voltage/Control/Electrical" pages)

Complete mapping from disassembly. Each item: query value + H/L limits, dial editor, write, SE commit.

| Menu Label | Value Reg | Hi/Lo Limits | SE Commit | Our Value |
|------------|-----------|-------------|-----------|-----------|
| VdLowLim | UW (0x5557) | WH / WL | SE=UW | 0 |
| VdRefON | BN (0x424E) | HN / ? | SE=BN | 0 |
| VdRefOFF | BF (0x4246) | HF / LF | SE=BF | 0 |
| SpdAdvMax | NC (0x4E43) | TC / SC | SE=NC | 0 |
| SpdRmp | **DN** (0x444E) | HD / LD | SE=DN | 1000 |
| TrqRmp | **SR** (0x5352) | HR / LR | SE=SR | 1000 |
| CurLim | CL (0x434C) | — | SE=CL | 100 |
| AdvMax | SA (0x5341) | — | SE=SA | 0 |
| PulseMax | SU (0x5355) | — | SE=SU | 50 |
| UVtSdStp | TS (0x5453) | — | SE=TS | 260 |
| UVtSdRun | UV (0x5556) | — | SE=UV | 200 |
| Ir Gain (low-level) | I0 (0x4930) | — | SE=I0 | 0 |
| Ir Offset (low-level) | I3 (0x4933) | — | SE=I3 | 0 |

**IMPORTANT name collision:** The deeper menu "TrqRmp" maps to **SR** (0x5352), while
the Motor Param menu "TrqRmp" maps to **TR** (0x5452). These are different registers!
Similarly, Ir Gain has both IU (Motor Param) and I0 (deeper), Ir Offset has OV and I3.

**Corrected FAQ mapping** (cross-referenced with disassembly):
| FAQ Label | FAQ Default | Correct MCB Reg | Our Value | Notes |
|-----------|-------------|-----------------|-----------|-------|
| UVtSdStp | 345v | TS (0x5453) | 260 | UV shutdown stop |
| UVtSdRun | 220v | UV (0x5556) | 200 | UV shutdown run |
| Ir Gain | 28835 | **IU** (0x4955) | 1000 | NOT I0! Disassembly confirms |
| Ir Offset | 82 | **OV** (0x4F56) | 450 | NOT I3! Disassembly confirms |
| VdLowLim | 300v | UW (0x5557) | 0 | — |
| VdRefON | 380v | BN (0x424E) | 0 | — |
| VdRefOFF | 360v | BF (0x4246) | 0 | — |
| SpdAdvMax | 1000 | NC (0x4E43) | 0 | — |
| SpdRmp | 1000/s | **DN** (0x444E) | 1000 | NOT SR! Disasm+query confirms DN=1000 |
| TrqRmp | 2000ms | TR (0x5452) | 75 | Very different |
| CurLim | 70% | CL (0x434C) | 100 | We set 100% at boot |
| AdvMax | 85 | SA (0x5341) | 0 | — |
| PulseMax | 185 | **PU** (0x5055) | 1000 | NOT SU! |

**Analysis of discrepancies:**

The drill press FAQ shows a fully-calibrated MCB with Ir compensation (I0=28835, I3=82),
voltage reference management (BN=380, BF=360), and advance control (SA=85, NC=1000).
Our MCB has all of these at **zero** — either:

1. **Different product calibration:** Our Voyager drill press motor may use different
   Striatech defaults than the FAQ's target unit
2. **Parameters were zeroed:** A factory reset or our testing may have cleared them
3. **GB1.7 firmware uses different internal defaults:** The MCB may apply built-in
   defaults when registers are 0, with explicit values only needed for non-standard motors

**Most likely explanation:** The FAQ is from January 2017 with an older MCB firmware.
Our GB1.7 is newer — Striatech likely moved calibration into the firmware itself, so
registers read 0 = "use firmware-internal default". The non-zero values in the FAQ were
explicit overrides needed by the older firmware. Our REGSCAN factory backup (above) is
the correct baseline for GB1.7 — do NOT apply the FAQ values to our MCB.

**CAUTION** from Teknatool: "Extreme changes to these parameters can result in
undesirable and potentially unsafe behaviour"

### Complete Command Reference (discovered 2026-01-24 via disassembly)

**Legend:** ✓ = captured/verified, ? = inferred from disassembly

#### Core Control Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| RS | 0x5253 | Stop/Brake | ✓ |
| ST | 0x5354 | Start motor | ✓ |
| JF | 0x4A46 | Direction (1706=FWD, 1707=REV, 3670/3669=JOG) | ✓ |
| SV | 0x5356 | Set/query velocity (RPM) | ✓ |
| GF | 0x4746 | Get flags/status | ✓ |
| GV | 0x4756 | Get MCB version | ✓ |
| CV | 0x4356 | Current velocity feedback | ✓ |
| KR | 0x4B52 | Keep running / load% | ✓ |
| MR | 0x4D52 | **Meaning UNKNOWN, and used by nobody.** Answers a query with 0 — stopped, running, and from the earliest observable moment of a cold boot. The OEM never sends it (zero MR frames in every logic-analyser capture; its disassembled wrapper is never called). "Motor Reset"/"Motor Ready" are both unverified guesses. See the cold-boot measurement below. | measured: always 0 |

### MR cold-boot measurement (2026-08-30)

The one condition never sampled was the MCB's own startup — a readiness flag
could only differ there. Measured with a temporary boot probe polling MR and GF
together from the first instruction of `task_motor`, on a true power-on:

```
=== MR COLD-BOOT PROBE (MR + GF) ===
[ 35 ms] MR=0  GF=32
[109 ms] MR=0  GF=32
[184 ms] MR=0  GF=32     ... unchanged through ...
[559 ms] MR=0  GF=32
```

**MR reads 0 from the earliest moment the HMI can ask.** The important detail is
that GF already answered 32 at that same 35 ms: **the MCB finishes its own
startup before our firmware even reaches the motor task** (LCD init and splash
run first; we do not query GV until 886 ms). So this is not "MR never changes" —
it is "we can never be early enough to see it change".

Settling it further needs a logic analyser on USART3 from the instant power is
applied, capturing what the MCB emits before anyone asks. The captures in
`captures/` were taken that way and contain no MR traffic at all.
| GR | 0x4752 | Grip/brake status (returns 3) | ✓ |
| MA | 0x4D41 | Motor angle (returns 0) | ✓ |

#### Speed Profiles (S0-S9)
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| S0 | 0x5330 | HARD (high torque) | ✓ |
| S1 | 0x5331 | Profile 1 | ? |
| S2 | 0x5332 | Secondary speed (900) | ✓ |
| S3-S6 | 0x5333-36 | Profiles 3-6 | ? |
| S7 | 0x5337 | NORMAL | ✓ |
| S8 | 0x5338 | SOFT (low torque) | ✓ |
| S9 | 0x5339 | Profile 9 | ? |

#### Voltage/Spindle Hold Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| V0 | 0x5630 | Voltage param 0 | ? |
| V1 | 0x5631 | Voltage param 1 | ? |
| V8 | 0x5638 | Voltage param 8 (264 in hold) | ✓ |
| VG | 0x5647 | Voltage gain (261 in hold) | ✓ |
| VR | 0x5652 | Voltage ramp (0-100) | ✓ |
| VS | 0x5653 | Voltage set (0/1) | ✓ |

#### Current/IR Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| CA | 0x4341 | Config register (0 stopped and running) | ✓ |
| CL | 0x434C | Current limit (70/100%) | ✓ |
| CU | 0x4355 | Config register (0 stopped and running) | ✓ |
| I0 | 0x4930 | IR compensation param 0 (always 0) | ✓ |
| I3 | 0x4933 | IR compensation param 3 (always 0) | ✓ |
| IH | 0x4948 | Current high threshold (9999=disabled) | ✓ |
| IL | 0x494C | Current limit factory (100) | ✓ |
| IU | 0x4955 | IR gain (1000=100.0%) | ✓ |

#### Temperature Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| T0 | 0x5430 | Heatsink temp °C (live — 18→19 when running) | ✓ |
| TC | 0x5443 | Temperature calibration offset (0) | ✓ |
| TH | 0x5448 | Thermal high shutdown (260) | ✓ |
| TL | 0x544C | Thermal low threshold (200) | ✓ |
| TS | 0x5453 | Thermal sensor — same as TH (260) | ✓ |
| HT | 0x4854 | Heat threshold (75°C) | ✓ |
| LT | 0x4C54 | Low temp threshold (4°C freeze protect) | ✓ |

#### Brake Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| BF | 0x4246 | Brake forward (returns 0) | ✓ |
| BN | 0x424E | Brake normal (returns 0) | ✓ |
| BR | 0x4252 | Brake mode | ✓ |

#### Under-Voltage/Warning Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| UD | 0x5544 | DC bus voltage — live (356-357V) | ✓ |
| UH | 0x5548 | UV high threshold (9999=disabled) | ✓ |
| UL | 0x554C | UV low threshold (100V) | ✓ |
| UV | 0x5556 | UV run threshold (200V = UVtSdRun) | ✓ |
| UW | 0x5557 | UV warning (0=none) | ✓ |
| WH | 0x5748 | Warning high (0=disabled) | ✓ |
| WL | 0x574C | Warning low (0=disabled) | ✓ |

#### High/Low Threshold Pairs (Hx/Lx) — all verified via REGSCAN
| Pair | High | Low | Meaning |
|------|------|-----|---------|
| HA/LA | 0/0 | | Advance (0 = SRM, no field advance) |
| HD/LD | 1000/50 | | Duty max 100% / Load threshold 50% |
| HF/LF | 0/0 | | Frequency (0 = SRM, no AC frequency) |
| HI/LI | 9999/10 | | Current: hi=sentinel / lo=10% |
| HL/LL | 100/20 | | Speed range: 20-100% of max |
| HM/LM | 200/150 | | Motor operating range |
| HN/LN | 0/0 | | Unknown (both zero) |
| HO/LO | 450/400 | | IR offset range (OV=450 is within) |
| HP/LP | 9999/10 | | HW alert=sentinel / LP=config(10) |
| HR/LR | 10000/1000 | | Ramp: 10s max / 1s min |
| HU/LU | 100/10 | | Unknown limit range |
| HV/LV | 6000/50 | | Voltage: 600.0V max / 5.0V min |

#### EEPROM Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| EE | 0x4545 | EEPROM execute — used as SE=EE in factory reset | ✓ |
| EU | 0x4555 | Dead code in original FW — never queried (0) | ✓ |
| EV | 0x4556 | Rotor electrical position (0-255) — dead code in original FW | ✓ |

**Note:** SP (0x5350) is NOT "Save Parameters" - it's Kprop! See PID section below.

### MCB Factory Reset Sequence (confirmed from disassembly 2026-05-26)

Function at `0x801ac04`, called from `0x80048ea`. After reset completes, firmware
enters infinite loop — requires power cycle.

**Complete command sequence (21 commands):**
```
RS=1              → EEPROM write-enable / reset prep
SE=EE             → Factory erase trigger
SE=S0             → Reset speed profile slot 0 (HARD)
SE=S1             → Reset speed profile slot 1
SE=S2             → Reset speed profile slot 2
SE=S3             → Reset speed profile slot 3
SE=S4             → Reset speed profile slot 4
SE=S5             → Reset speed profile slot 5
SE=S6             → Reset speed profile slot 6
SE=S7             → Reset speed profile slot 7 (NORMAL)
SE=S8             → Reset speed profile slot 8 (SOFT)
SE=S9             → Reset speed profile slot 9
SE=TR             → Reset torque ramp
SE=FD             → Reset profile select
SE=SP             → Reset Kprop
SE=SI             → Reset Kint
SE=SR             → Reset speed ramp (SR register)
SE=DN             → Reset SpdRmp (DN register — the REAL speed ramp)
SE=CL             → Reset current limit
SE=NC             → Reset SpdAdvMax
SE=SA             → Reset AdvMax
SE=SU             → Reset SU
SE=BN             → Reset VdRefON
SE=BF             → Reset VdRefOFF
SE=UW             → Reset VdLowLim
SE=PU             → Reset PulseMax
SE=IU             → Reset Ir Gain
SE=OV             → Reset Ir Offset
SE=UV             → Reset UV threshold
```
→ Infinite loop (MCU halts, power cycle required)

**DN register (0x444E)** — "SpdRmp" in service menu. Missing from original REGSCAN
list but responds to query: DN=1000. This is the actual speed ramp parameter, distinct
from SR (0x5352) which the disassembly shows as a separate ramp parameter.

**Trigger path:** Service menu flag 0x200 bit 2 → LCD update → `0x801ac04` → halt.
The trigger is likely the "Reset" confirmation in the service menu.

**Notes:**
- RS=1 is ONLY used before EE — not a general reset mode
- After RS=1 + SE=EE, the MCB enters a write-back mode where each subsequent SE
  commit restores that register to factory default
- MCB goes silent during reset (~1.5s), doesn't respond to queries
- The earlier capture-based documentation (RS=1 × 6, 0x5200 binary) was a decoder
  artifact — the actual sequence is clean: 1 RS=1 + 20 SE commits

#### Misc Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| F0 | 0x4630 | Fault code (15=no fault, 0-14=fault codes) | ✓ |
| FD | 0x4644 | Profile select (0=Normal, 1=Soft, 2=Hard) | ✓ |
| NC | 0x4E43 | Normal check / SpdAdvMax (0) | ✓ |
| SC | 0x5343 | Speed control mode (0) | ✓ |
| **SE** | **0x5345** | **Set Enable - commit parameter changes** | **✓** |
| SI | 0x5349 | Kint — integral gain (500=50%) | ✓ |
| SL | 0x534C | Speed limit (0=none, 10=during hold) | ✓ |
| SU | 0x5355 | PulseMax (50 in service menu) | ✓ |
| SX | 0x5358 | AC tapping param (0 default, 800 in menu) | ✓ |

**Total: 98 registers queried** (all respond, 0 silent — see register dump below)

### Service Menu Parameter Mapping (discovered 2026-01-25)

Complete mapping of service menu items to MCB commands, captured via logic analyzer:

| Menu Item | Command | Hex | Default Value | Notes |
|-----------|---------|-----|---------------|-------|
| **Ir Gain** | IU | 0x4955 | 1000 | IR compensation gain (disasm confirms, NOT I0) |
| **Ir Offset** | OV | 0x4F56 | 450 | IR compensation offset (disasm confirms, NOT I3) |
| **VdLowLim** | UW | 0x5557 | 0 | Vd low limit |
| **VdRefOn** | BN | 0x424E | 0 | Vd reference on |
| **VdRefOff** | BF | 0x4246 | 0 | Vd reference off |
| **SpdAdvMax** | NC | 0x4E43 | 0 | Speed advance max |
| **SpdRmp** | DN | 0x444E | 1000 | Speed ramp rate |
| **TrqRmp** | SR | 0x5352 | 1000 | Torque ramp rate |
| **CurLim** | CL | 0x434C | 100 | Current limit % |
| **AdvMax** | SA | 0x5341 | 0 | Advance max |
| **PulseMax** | PU | 0x5055 | 1000 | PWM pulse max (disasm confirms, NOT SU) |
| **UVtSdStp** | TS | 0x5453 | 260 | Undervoltage stop (V) |
| **UVtSdRun** | UV | 0x5556 | 200 | Undervoltage run (V) |
| **Kprop** | SP | 0x5350 | 1000 | **Proportional gain (100%)** |
| **Kint** | SI | 0x5349 | 500 | **Integral gain (50%)** |
| **Profile** | FD | 0x4644 | 0 | 0=Normal, 1=Soft, 2=Hard |
| **DC Bus** | UD | 0x5544 | ~356 | DC bus voltage (V) |
| **T HtSink** | T0 | 0x5430 | ~26 | Heatsink temp (°C) |
| **AC-Tapping** | SX | 0x5358 | 800 | AC tapping parameter |

**Critical Discovery:** SP and SI are the **real** PID parameters!
- SP (0x5350) = Kprop = Proportional gain (NOT "Save Parameters"!)
- SI (0x5349) = Kint = Integral gain
- VP (0x5650) and VI (0x5649) may be unused or different parameters

### SE Command Format (discovered 2026-01-25)

The SE (Set Enable, 0x5345) command commits parameter changes to RAM.
**SE takes the parameter's command code as its value!**

```
I3=5              → Set IR Offset to 5
SE=I3             → Commit I3 (sends SE with param 0x4933)
                  → motor_send_command(CMD_SE, 0x4933)
I3?               → Query to verify
```

**Pattern for setting parameters:**
```c
motor_send_command(CMD_I3, 5);           // Set value
motor_send_command(CMD_SE, CMD_I3);      // Commit (SE=0x4933)
motor_read_param(CMD_I3);                // Verify
```

### Factory Reset Sequence (updated 2026-01-25)

Factory reset uses repeated RS=1 commands, NOT the EE command:

```
RS=1 × N          → Set "reset pending" flag
[power cycle]     → MCB resets EEPROM on next boot
```

**Note:** MCB parameters appear to be **factory-programmed and read-only**.
Testing on both R2P05x (May 2018) and R2P06K (latest) showed the same behavior:
- Setting parameters (e.g., I3=12) sends the command
- SE=I3 commits to RAM
- Query returns 0 (unchanged)

This suggests the MCB EEPROM is write-protected at hardware level.

### R2P05x vs R2P06K Boot Sequence Differences

**R2P05x (simpler):**
```
RS=0 × 4-5         → Stop commands
JF=1706            → Set forward
SV? → SV=900       → Query/confirm speed
S2? → 900          → Query Speed2
CL? → 100          → Query current limit
```

**R2P06K (more extensive):**
```
RS=0 × 3           → Stop
KR? GF? GF?        → Status poll
RS=0               → Another stop
GV?                → Query MCB version (not in R2P05x)
...polling...      → Continuous KR/GF checks
JF=1706, SV sync, S2?, CL?
```

Key differences:
- R2P05x doesn't query GV (MCB version)
- R2P05x has fewer stop commands
- R2P06K has more extensive status polling

### Important Implication
**Motor control is NOT direct GPIO/PWM** - all commands go via serial to the motor controller.
This makes patching easier: just call the existing motor_forward()/motor_reverse() functions.

### Complete Register Dump (REGSCAN 2026-05-26)

All 98 known registers queried via motor_test firmware REGSCAN command.
MCB firmware GB1.7. 98/98 responded, 0 silent.

**Key: Δ = value changes when motor is running**

#### Core Status
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| GF | 32 | **34** Δ | Status flags (32=stop fwd, 34=run fwd) |
| F0 | 15 | 15 | Fault code (15=no fault/idle default) |
| GV | "GB1.7" | "GB1.7" | MCB firmware version |
| GR | 5 | **6** Δ | RPS sensor bitmask (A=1,B=2,C=4) — also brake status |
| FD | 0 | 0 | Fault detect / profile (0=Normal) |
| MR | 0 | 0 | **Unknown** — reads 0 in both columns; the "Motor ready" label is a guess from a test script, not from the MCB |
| RP | 0 | 0 | Rotor position |
| PH | 0 | 0 | Phase (Hall sensor) |
| NC | 0 | 0 | Normal check |
| HP | 9999 | 9999 | Hardware alert threshold (9999=disabled) |
| F0 note: returns 15 ("Idle") even while motor runs — 15 = no active fault, not literally idle |

#### Speed / Velocity
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| SV | 500 | 500 | Target speed (RPM) |
| CV | 0 | **499** Δ | Actual speed (RPM) — live feedback |
| S2 | 500 | 500 | Secondary speed param (mirrors SV) |
| SL | 0 | 0 | Speed limit (0=none; 10 during spindle hold) |
| SC | 0 | 0 | Speed control mode |
| SU | 50 | 50 | PulseMax (service menu "PulseMax") |
| SX | 0 | 0 | AC tapping param (800 = default in service menu) |

#### Load / Current — RESOLVED
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| **KR** | **0** | **12** Δ | **Motor load % — THE LIVE LOAD REGISTER** |
| LP | 10 | 10 | Static config value, NOT live load |
| LD | 50 | 50 | Load threshold — overload trip point (50%) |
| CL | 100 | 100 | Runtime current limit (user: 20/50/70/100) |
| IL | 100 | 100 | Factory current limit max |
| CA | 0 | 0 | Not live current — config register |
| CU | 0 | 0 | Not live current — config register |
| I0 | 0 | 0 | IR compensation param 0 |
| I3 | 0 | 0 | IR compensation param 3 |
| IH | 9999 | 9999 | Current high threshold (9999=disabled) |
| IU | 1000 | 1000 | IR gain (100.0%) |
| OV | 450 | 450 | IR offset |

**KR mystery solved (2026-05-25):** KR query works correctly in bare-metal motor_test
firmware — returns 0 stopped, 12 running at 500 RPM unloaded. The previous "always 0"
bug was in the main FreeRTOS firmware (timing/mutex issue in task_motor.c), not a
protocol problem. LP is NOT a live load register despite the config.h comment — it's
a static value (10) that doesn't change with motor state.

#### PWM / Duty
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| PU | 1000 | 1000 | Pulse max (100.0%) |
| PW | 0 | 0 | Pulse width (0 = auto/controlled by MCB) |
| HD | 1000 | 1000 | Duty high limit (100.0%) |

#### Voltage
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| UD | 356 | **357** Δ | DC bus voltage — live reading |
| V0 | 0 | 0 | Voltage param 0 (spindle hold) |
| V1 | 0 | 0 | Voltage param 1 (spindle hold) |
| V8 | 0 | 0 | Voltage param 8 (264 during hold) |
| VR | 0 | 0 | Voltage ramp (0=off, 100=hold) |
| VS | 0 | 0 | Voltage set (0=off, 1=hold) |
| VG | 0 | 0 | Voltage gain (261 during hold) |
| VP | 0 | 0 | Voltage Kp (PID) |
| VI | 0 | 0 | Voltage Ki (PID) |

#### Thermal
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| T0 | 18 | **19** Δ | Heatsink temp °C — live, rose 1°C |
| HT | 75 | 75 | Heat threshold (75°C = fan trigger?) |
| TH | 260 | 260 | Thermal high shutdown (260V AC?) |
| TL | 200 | 200 | Thermal low threshold |
| TS | 260 | 260 | Thermal sensor = TH (same register?) |
| TC | 0 | 0 | Temp calibration offset |
| LT | 4 | 4 | Low temp threshold (4°C freeze protect) |

#### Ramp / PID
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| SR | 1000 | 1000 | Speed ramp rate (ms to target) |
| TR | 75 | 75 | Torque ramp rate |
| SP | 1000 | 1000 | Kprop (proportional gain, 100.0%) |
| SI | 500 | 500 | Kint (integral gain, 50.0%) |
| KP | 0 | 0 | Alternate Kp (unused, 0) |
| KI | 0 | 0 | Alternate Ki (unused, 0) |
| SA | 0 | 0 | Advance max |
| MA | 0 | 0 | Motor angle |

#### H/L Threshold Pairs
| Pair | High | Low | Likely meaning |
|------|------|-----|----------------|
| HA/LA | 0/0 | | Advance high/low (both 0 = SRM, no field advance) |
| HD/LD | 1000/50 | | Duty hi 100% / Load threshold 50% |
| HF/LF | 0/0 | | Frequency (0 = SRM, no AC frequency) |
| HI/LI | 9999/10 | | Current hi sentinel / Current low 10% |
| HL/LL | 100/20 | | Speed limit range: 20-100% of max RPM |
| HM/LM | 200/150 | | Motor operating range (RPM? electrical?) |
| HN/LN | 0/0 | | Unknown (zeroed) |
| HO/LO | 450/400 | | IR offset operating range (matches OV=450) |
| HP/LP | 9999/10 | | HW alert sentinel / "Load percentage" config |
| HR/LR | 10000/1000 | | Ramp range (10s max, 1s min) |
| HU/LU | 100/10 | | Unknown limit (100/10) |
| HV/LV | 6000/50 | | Voltage range (600.0V max / 5.0V min) |

#### Brake
| Reg | Value | Description |
|-----|-------|-------------|
| BR | 0 | Brake mode (0=coast?) |
| BF | 0 | Brake forward |
| BN | 0 | Brake normal (VdRefOff in service menu) |

#### Under-Voltage / Warning
| Reg | Value | Description |
|-----|-------|-------------|
| UD | 356-357 | DC bus voltage — live |
| UH | 9999 | UV high threshold (sentinel) |
| UL | 100 | UV low threshold (100V) |
| UV | 200 | UV run threshold (200V = UVtSdRun) |
| UW | 0 | UV warning (0=none) |
| WH | 0 | Warning high |
| WL | 0 | Warning low |

#### Speed Profiles (S0-S9)
| Reg | Value | Description |
|-----|-------|-------------|
| S0 | 2500 | HARD profile (max RPM) |
| S1 | 250 | Profile 1 (min RPM?) |
| S2 | 500 | Secondary speed (mirrors SV) |
| S3 | 750 | Profile 3 |
| S4 | 1000 | Profile 4 |
| S5 | 1250 | Profile 5 |
| S6 | 1500 | Profile 6 |
| S7 | 750 | NORMAL profile |
| S8 | 2000 | SOFT profile |
| S9 | 2250 | Profile 9 |

S0-S9 appear to be speed presets in the MCB, NOT torque profiles. S0 "HARD"=2500, S7
"NORMAL"=750, S8 "SOFT"=2000 — the naming doesn't correlate with the values. These may
be configurable via the service menu.

#### EEPROM
| Reg | Stopped | Running @500 | Description |
|-----|---------|-------------|-------------|
| EE | 0 | 0 | EEPROM execute (SE=EE in factory reset) |
| EU | 0 | 0 | Unknown (dead code in original FW) |
| EV | varies | varies | Rotor electrical position (0-255) |

EV is likely a **rotor electrical position** register (0-255, wrapping). Observed values
correlate with spindle stop position: 253→255 (close stops), 0 while spinning (caught
mid-rotation). Not a counter or version. Disassembly confirms EV query is DEAD CODE in
both R2P06K and R2P05x — Teknatool never reads it.

#### Registers that change while motor runs (Δ summary)
| Reg | Stopped | Running | Notes |
|-----|---------|---------|-------|
| GF | 32 | 34 | Status flags |
| GR | 5 | 6 | Brake status |
| CV | 0 | 499 | Actual RPM |
| KR | 0 | 12 | Load % |
| T0 | 18 | 19 | Heatsink temp |
| UD | 356 | 357 | DC bus voltage |
| EV | varies | varies | Rotor electrical position (0-255) — dead code in original FW |

Only 6 meaningful dynamic registers (+EV rotor position). The rest are configuration.

### MCB Manufacturer: Striatech (confirmed 2026-05-26)

The MCB is manufactured by **Striatech** (striatech.com), a company specializing in
DVR (Digital Variable Reluctance) switched reluctance motor systems. Teknatool/NOVA is
the OEM customer. The motor+controller is a Striatech product with custom firmware (GB1.7).

- Striatech offers SRM motors from 1-3 HP
- Nova Voyager uses 1.75HP (110V) / 2HP (220V) variant
- MCB has built-in USB for Striatech firmware updates (separate from HMI)
- No public protocol documentation — everything reverse-engineered

### MCB Register Writability (confirmed 2026-05-26)

**MCB EEPROM is writable** from the HMI via the serial protocol. Changes persist
across power cycles.

**Write + commit sequence:**
```
MC <XX> <val>     → Set register in MCB RAM (gets ACK)
SE=<XX>           → Commit to MCB EEPROM (SE with raw cmd bytes)
MQ <XX>           → Verify readback
```

**SE packet format** (from disassembly at 0x801b23a — different from normal commands):
```
[SOH][00][00][11][11][STX][1][S][E][target_H][target_L][ETX][XOR]
```
The target command code is sent as **raw bytes**, not ASCII decimal. Both formats
(raw and ASCII decimal) are ACKed by GB1.7, but raw matches the original firmware.

**Tested:** SET S7 750→500 (ACK, verified, persisted across reset, restored to 750).

**Writable registers (confirmed or expected):**
- S0-S9: Speed profiles
- SP, SI: PID proportional/integral gains
- SR, TR: Speed/torque ramp rates
- CL: Current limit
- FD: Profile select (Normal/Soft/Hard)
- I0, I3, IU, OV: IR compensation parameters
- All H/L threshold pairs
- BF, BN, BR: Brake parameters
- V8, VG, VR, VS: Spindle hold voltage parameters

### MCB Factory Defaults Backup (REGSCAN 2026-05-26, stopped, post-restore)

Reference values for restoring MCB to known-good state. **Confirmed factory defaults
for GB1.7** — all 27 factory-reset registers verified as clean/unmodified (2026-05-26).
No prior service calibration has been applied to this MCB.

```
# Core status (live — values shown are typical stopped state)
GF=32  F0=15  GV=GB1.7  GR=14  MR=0  RP=0  PH=0  NC=0  FD=0  HP=9999

# Speed
SV=500  CV=0  S2=900  SL=0  SC=0  SU=50  SX=0

# Speed profiles
S0=2500  S1=250  S3=750  S4=1000  S5=1250  S6=1500  S7=750  S8=2000  S9=2250

# Load / Current
KR=0  LP=10  LD=50  CL=100  IL=100  CA=0  CU=0
I0=0  I3=0  IH=9999  IU=1000  OV=450

# PWM / Duty
PU=1000  PW=0  HD=1000

# Voltage
UD=358  V0=0  V1=0  V8=0  VR=0  VS=0  VG=0  VP=0  VI=0

# Thermal
T0=24  HT=75  TH=260  TL=200  TS=260  TC=0  LT=4

# Ramp / PID
SR=1000  TR=75  SP=1000  SI=500  KP=0  KI=0  SA=0  MA=0

# H/L threshold pairs
HA=0  LA=0  HD=1000  LD=50  HF=0  LF=0
HI=9999  LI=10  HL=100  LL=20  HM=200  LM=150
HN=0  LN=0  HO=450  LO=400  HR=10000  LR=1000
HU=100  LU=10  HV=6000  LV=50

# Brake
BR=0  BF=0  BN=0

# Under-voltage / Warning
UD=358  UH=9999  UL=100  UV=200  UW=0  WH=0  WL=0

# EEPROM
EE=0  EU=0  EV=0

# Misc
SE=0
DN=1000
```

### Factory Default Verification (2026-05-26)

All 27 registers in the factory reset sequence (RS=1 + SE=EE + 20× SE=XX) were
queried and confirmed as unmodified factory defaults for GB1.7:

```
# Factory reset registers — all at GB1.7 defaults
S0=2500  S1=250   S2=900   S3=750   S4=1000
S5=1250  S6=1500  S7=750   S8=2000  S9=2250
TR=75    FD=0     SP=1000  SI=500   SR=1000  DN=1000
CL=100   NC=0     SA=0     SU=50
BN=0     BF=0     UW=0
PU=1000  IU=1000  OV=450   UV=200
```

All values are clean round numbers. Speed profiles form a natural progression.
PID gains at standard percentages (SP=100%, SI=50%). Voltage reference and advance
registers zeroed (GB1.7 uses firmware-internal defaults for these).

**Conclusion:** This MCB has never been recalibrated from Striatech factory settings.

---


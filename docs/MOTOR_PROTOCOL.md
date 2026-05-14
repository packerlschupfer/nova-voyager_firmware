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

### Fault Codes (F0 query response)

**Motor type: Switched Reluctance Motor (SRM)** — confirmed from MCB firmware strings.
The MCB uses a Rotor Position Sensor (RPS) for commutation and has a PFC stage.

F0=15 is the MCB's idle/default response meaning **no active fault**. F0=0–14 and 50/55/56
are only returned when GF bit 14 is set (error state). Confirmed from MCB firmware disassembly
(2026-03-03).

| Code | Meaning | Notes |
|------|---------|-------|
| 0  | Unexpected Fault / Control Board Issue | May require servicing |
| 1  | SRM Not Rotate | Check motor connection; check drill can rotate |
| 2  | RPS State Error 0 | Rotor Position Sensor — check RPS connection |
| 3  | RPS State Error 1 | Rotor Position Sensor fault |
| 4  | Hardware Fault | — |
| 5  | Unexpected Error | — |
| 6–12 | Unknown | No specific message in MCB firmware |
| 13 | Low Voltage (UVL) | Triggered on power-down; resets to 15 after recovery |
| 14 | PFC Fault | ⚠ NOT "Motor Lock" — Power Factor Correction stage fault |
| 15 | (no active fault) | MCB idle default; always returned when no error |
| 50 | Inverter Overheated | — |
| 55 | EEPROM Data Fault | — |
| 56 | EEPROM Error | — |

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
[1.61s] ...polling...   → KR? GF? GF? every ~350ms
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
```

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
- Poll pattern when running: KR? GF? GF? CV? every ~350ms

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
| MR | 0x4D52 | Motor reset/ready query | ? |
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
| CA | 0x4341 | Current actual | ? |
| CL | 0x434C | Current limit (70/100%) | ✓ |
| CU | 0x4355 | Current ? | ? |
| I0 | 0x4930 | IR param 0 | ? |
| I3 | 0x4933 | IR param 3 | ? |
| IH | 0x4948 | Current high threshold | ? |
| IL | 0x494C | Current limit factory | ✓ |
| IU | 0x4955 | IR gain | ✓ |

#### Temperature Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| T0 | 0x5430 | Temperature baseline | ✓ |
| TC | 0x5443 | Temperature calibration (returns 0) | ✓ |
| TH | 0x5448 | Temperature high threshold | ✓ |
| TL | 0x544C | Temperature low threshold | ✓ |
| TS | 0x5453 | Temperature sensor | ? |
| HT | 0x4854 | Heat/thermal query | ✓ |
| LT | 0x4C54 | Low temperature | ? |

#### Brake Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| BF | 0x4246 | Brake forward (returns 0) | ✓ |
| BN | 0x424E | Brake normal (returns 0) | ✓ |
| BR | 0x4252 | Brake mode | ✓ |

#### Under-Voltage/Warning Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| UD | 0x5544 | Under-voltage detect | ? |
| UH | 0x5548 | Under-voltage high | ? |
| UL | 0x554C | Under-voltage low | ? |
| UV | 0x5556 | Under-voltage value | ? |
| UW | 0x5557 | Under-voltage warning (returns 0) | ✓ |
| WH | 0x5748 | Warning high (returns 0) | ✓ |
| WL | 0x574C | Warning low (returns 0) | ✓ |

#### High/Low Threshold Pairs (Hx/Lx)
| Pair | Codes | Description | Status |
|------|-------|-------------|--------|
| HA/LA | 0x4841/4C41 | Advance limits | ? |
| HD/LD | 0x4844/4C44 | Duty/Load limits | ✓LD |
| HF/LF | 0x4846/4C46 | Frequency limits | ? |
| HI/LI | 0x4849/4C49 | Current limits | ? |
| HL/LL | 0x484C/4C4C | General limits | ? |
| HM/LM | 0x484D/4C4D | Motor limits | ? |
| HN/LN | 0x484E/4C4E | ? limits | ? |
| HO/LO | 0x484F/4C4F | ? limits | ? |
| HP/LP | 0x4850/4C50 | Power/Load | ✓ |
| HR/LR | 0x4852/4C52 | Ramp limits | ? |
| HU/LU | 0x4855/4C55 | ? limits | ? |
| HV/LV | 0x4856/4C56 | Voltage limits | ? |

#### EEPROM Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| EE | 0x4545 | EEPROM execute | ✓ |
| EU | 0x4555 | EEPROM ? | ? |
| EV | 0x4556 | EEPROM version | ? |

**Note:** SP (0x5350) is NOT "Save Parameters" - it's Kprop! See PID section below.

### MCB Factory Reset Sequence (discovered 2026-01-25)

Triggered via service menu "Reset" confirm:

```
RS=1 × 6              → Prepare/enter reset mode
0x5200=S[00]1[00]     → Execute factory reset (binary command with nulls)
RS=1 × 7              → Wait/confirm sequence
KR? × 3 (no response) → MCB busy resetting (~0.7s)
KR=0, GF=32           → Reset complete, normal operation
```

**RS parameter values (confirmed via disassembly):**
| Value | Meaning | Firmware Location |
|-------|---------|-------------------|
| 0 | Normal stop/brake | 0x801a456 |
| 1 | EEPROM reset prep (used before EE command) | 0x801ac04 |

**Disassembly proof:**
```asm
; RS=0 - normal stop
801a458:  movs r1, #0         ; parameter = 0
801a45a:  movw r0, #0x5253    ; RS command
801a45e:  bl   motor_cmd

; RS=1 - EEPROM reset (immediately followed by EE)
801ac06:  movs r1, #1         ; parameter = 1
801ac08:  movw r0, #0x5253    ; RS command
801ac0c:  bl   motor_cmd
801ac10:  movw r0, #0x4545    ; EE command (EEPROM Execute)
801ac14:  bl   eeprom_cmd
```

**Notes:**
- RS=1 is NOT a general "reset mode" - only used before EE (EEPROM Execute)
- The 0x5200 command in captures uses null bytes - may be decoder artifact
- MCB goes silent during reset, doesn't respond to queries
- Total reset time: ~1.5s from first RS=1 to MCB responding
- No other RS values exist in firmware (only 0 and 1)

#### Misc Commands
| Cmd | Code | Description | Status |
|-----|------|-------------|--------|
| F0 | 0x4630 | Fault code query (returns 0-15) | ✓ |
| FD | 0x4644 | Fault detect | ? |
| NC | 0x4E43 | Normal check | ? |
| SC | 0x5343 | Speed control | ? |
| **SE** | **0x5345** | **Set Enable - commit parameter changes** | **✓** |
| SI | 0x5349 | Speed initial | ? |
| SL | 0x534C | Speed limit | ✓ |
| SU | 0x5355 | Speed ? | ? |
| SX | 0x5358 | Speed ? | ? |

**Total: 90+ commands identified** (39 verified, 52+ inferred from disassembly)

### Service Menu Parameter Mapping (discovered 2026-01-25)

Complete mapping of service menu items to MCB commands, captured via logic analyzer:

| Menu Item | Command | Hex | Default Value | Notes |
|-----------|---------|-----|---------------|-------|
| **Ir Gain** | I0 | 0x4930 | 0 | IR compensation gain |
| **Ir Offset** | I3 | 0x4933 | 0 | IR compensation offset |
| **VdLowLim** | UW | 0x5557 | 0 | Vd low limit |
| **VdRefOn** | BN | 0x424E | 0 | Vd reference on |
| **VdRefOff** | BF | 0x4246 | 0 | Vd reference off |
| **SpdAdvMax** | NC | 0x4E43 | 0 | Speed advance max |
| **SpdRmp** | DN | 0x444E | 1000 | Speed ramp rate |
| **TrqRmp** | SR | 0x5352 | 1000 | Torque ramp rate |
| **CurLim** | CL | 0x434C | 100 | Current limit % |
| **AdvMax** | SA | 0x5341 | 0 | Advance max |
| **PulseMax** | SU | 0x5355 | 50 | PWM pulse max |
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

---


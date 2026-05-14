# Tapping Mode Analysis Reference
## Tapping/Peck Drilling Analysis

### Original Teknatool Tapping Modes (from FAQ)

**Important**: Tapping recommended for 5/16" (M8) and larger taps. Smaller taps may break.
User must manually feed the tap (apply quill pressure). Only works starting in Forward direction.

#### Mode 1: Load Sensing (Through-Hole Only)
| Phase | Description |
|-------|-------------|
| Pre-tapping | Motor starts, learns baseline "non-tapping" load at current speed |
| Tapping | Detects load increase → knows tapping started, monitors continuously |
| Reverse Out | Load drop detected (tap exited hole) → reverses out |

**NOT for blind holes** - relies on load drop when tap exits material.

#### Mode 2: Chip Breaker (Peck-Style)
| Phase | Description |
|-------|-------------|
| Pre-tapping | Same baseline learning as Load Sensing |
| Tapping | Fixed time forward (taps threads) |
| Chip Breaking | Fixed time reverse (clears chips) |
| Load Compare | If FWD load >> REV load → still tapping, repeat cycle |
| Reverse Out | If REV load ≈ FWD load → not tapping, reverse out |

**Tip for blind holes (R2P05 workaround):**
- Use "User Set Depth" to set tapping depth
- Set "Set Depth Reached" = "Stop and Rev (top)"
- Provides automatic reverse at depth without load sensing

#### Operational Notes
- **ON button** during tapping → immediate reverse-out
- **Power output** should be LOW for tapping (reduces tap breakage risk)
- **Load spike** threshold configurable in Drill Settings/Motor Performance
- Chuck slippage NOT detected - consider flats on tap shank

### Tapping Mode Analysis (logic analyzer captures 2026-01-25)

Captured actual tapping behavior at SV=100 RPM.

#### Load Sensing Mode - Working Behavior

**Clean capture timeline:**
```
[2.66s]  ST=0              → Forward start
[2.95s]  KR=17%            → Spin-up current
[3.07s]  CV=18             → Motor starting
[3.41s]  CV=105, KR=7%     → Reached target, settling
[3.66s]  KR=6%, CV=100     → BASELINE ESTABLISHED
         ...stable for ~4 seconds (learning phase)...
[8.05s]  KR=21%, CV=84     → LOAD DETECTED (tap engaged)
[8.42s]  KR=17%, CV=111    → Partial recovery
[8.79s]  KR=4%, CV=111     → LOAD RELEASED
[9.24s]  KR=6%, CV=136     → CV OVERSHOOT (through-hole exit!)
[9.41s]  RS=0              → AUTO-REVERSE TRIGGERED
[9.47s]  JF=1707 (REV)     → Set reverse
[9.54s]  ST=0              → Start reverse
[9.80s]  RS=0              → Stop (brief reverse complete)
[9.83s]  SV=450            → Restore main drilling speed
[9.90s]  JF=1706 (FWD)     → Reset to forward
```

**Load sensing detection mechanism:**
| Phase | KR | CV | Trigger |
|-------|----|----|---------|
| Baseline | 6% | 100 | - |
| Loaded | 17-21% | 84 | KR spike (3.5× baseline) |
| Through-hole | 4-6% | 136 | CV overshoot (36% over target) |

**Key insight:** Through-hole detection uses **CV overshoot**, not KR drop. When tap
exits material, load suddenly drops and motor overshoots target speed. HMI detects
CV=136 (36% above SV=100) and triggers reverse.

**Brief reverse phase:** Only ~0.26s reverse (9.54→9.80s) because tap already exited
hole. Just needs brief reverse to fully clear threads.

**Speed restoration:** After tapping complete, SV restored from 100→450 (main drilling speed).

#### Chip Breaker Mode - Works via Fixed Timing

**Capture timeline:**
```
[0.55s]  ST=0              → Forward start
[6.86s]  RS→JF=1707→ST     → REVERSE (chip break)
[8.32s]  RS→JF=1706→ST     → FORWARD (resume)
```

**Timing (fixed, not load-based):**
| Phase | Duration | Notes |
|-------|----------|-------|
| Forward | ~6.3s | Tapping |
| Reverse | ~1.5s | Chip clearing |

**KR at direction changes:**
```
[7.14s] KR=99%   (reverse start - high load)
[8.61s] KR=100%  (forward restart - high load)
```

**CV overshoot when load released:**
```
[9.51s] KR=6%  → CV=186 RPM (target 100!)
[9.87s] KR=3%  → CV=237 RPM (2.4x overshoot)
```
Motor overcompensates when load suddenly drops.

#### Tapping Mode Comparison

| Mode | Auto-Reverse? | Trigger | Through-Hole? | Blind Hole? |
|------|---------------|---------|---------------|-------------|
| Load Sensing | Yes | CV overshoot (36%+) | ✓ | ✗ |
| Chip Breaker | Yes | Fixed timer (~6s) | ✓ | ✓ |

**Load Sensing works via CV overshoot detection:**
- Baseline: KR=6%, CV=100 (stable for ~4s learning phase)
- Load applied: KR spikes to 17-21%, CV drops to 84
- Through-hole exit: CV overshoots to 136 (36% over target) → triggers reverse
- Brief reverse (~0.26s) then stop

**Chip Breaker for blind holes:** Uses fixed timing (6.3s fwd, 1.5s rev) instead
of through-hole detection. Works regardless of hole depth.

**Key insight:** Load sensing relies on **motor overshoot** when tap exits material,
NOT on KR threshold detection. This only works for through-holes where the tap
exits the workpiece.

#### Depth Stop with Reverse - Works Cleanly at Higher Speed

**Capture:** Depth stop mode at SV=450 RPM with reverse-on-depth-reached.

**Timeline:**
```
[0.00s]  ST=0              → Forward start
[0.26s]  KR=100%           → Full current during spin-up
[0.62s]  CV=0 → 311        → Motor spinning up
[1.35s]  CV=447, KR=11%    → Stable at target speed (99.3%)
         ...drilling...
[5.22s]  CV×3 burst        → HMI queries CV rapidly (depth decision)
[5.26s]  RS=0 × 2          → STOP (depth reached)
[5.43s]  JF=1707           → Set REVERSE
[5.47s]  ST=0              → Start reverse
[5.73s]  KR=100%           → Full current (reverse spin-up)
         ...reversing (no CV queries)...
[10.48s] RS=0 × 2          → STOP (quill at top)
[10.66s] JF=1706           → Reset to FORWARD
         ...post-stop re-sync sequence...
```

**Key observations:**
| Aspect | Value | Notes |
|--------|-------|-------|
| Target speed | 450 RPM | Higher than tapping test |
| Actual CV | 447-448 RPM | 99.3% of target (stable) |
| KR unloaded | 11% | Stable baseline |
| Forward time | ~5.2s | Until depth reached |
| Reverse time | ~5.0s | Until quill at top |
| CV queries | Burst before depth stop | 3× rapid queries, then decision |
| CV during reverse | Not queried | HMI doesn't monitor reverse speed |

**CV Burst Pattern:** Before stopping at depth, HMI sends 3× rapid CV queries
(~50ms apart) to verify motor speed. This appears to be a "confidence check"
before committing to the direction change.

**Reverse Phase:** Motor reverses without CV monitoring. HMI only checks GF/KR
during reverse, trusting the motor to reach target speed. This is acceptable
since reverse-out doesn't require precise speed control.

**Depth Stop vs Load Sensing:**
| Mode | Trigger | Use Case |
|------|---------|----------|
| Depth Stop | Absolute position | Blind holes to exact depth |
| Load Sensing | CV overshoot | Through-holes only |

Depth stop is more reliable for blind holes since it doesn't require detecting
when the tap exits material.

### Firmware String Locations
| Mode | String Offset | Description |
|------|---------------|-------------|
| OFF | 0x0800A5A0 | No automatic behavior |
| Load Sensing | 0x0800A7CC | Through-hole tapping |
| Chip Breaker | 0x0800A7E0 | Peck-style chip clearing |

### Key UI Strings
- "Tapping Mode" - 0x0800A5EC
- "Tapping Speed" - 0x0800A83C
- "START TAPPING" - 0x08012C0C
- "Tapping II" / "AC Tapping" - 0x08012D04 / 0x08016FF4 (unimplemented)

### State Variables (SRAM)
| Address | References | Likely Purpose |
|---------|------------|----------------|
| 0x2000002C | 61 | Main operating mode/state |
| 0x20000050 | 13 | Speed/PWM value |
| 0x20000080 | 11 | Target depth |
| 0x20000084 | 13 | Current depth/position |

### Motor Control
- TIM1 (0x40012C00): Main motor PWM - 7 references
- TIM2 (0x40000000): Secondary timer - 17 references
- Direction control via GPIO (Rechtslauf/Linkslauf)

---


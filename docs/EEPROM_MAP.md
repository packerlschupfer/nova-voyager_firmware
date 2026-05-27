# HMI EEPROM Map — AT24C02 on PC4(SCL)/PC5(SDA)

Nova Voyager DVR Drill Press — Teknatool firmware "DRIL5.20D_DP"

## Hardware

- **Chip**: AT24C02 (256 bytes, 8-byte pages, 1-byte addressing)
- **I2C bus**: PC4 = SCL, PC5 = SDA (bit-bang, no HAL)
- **Address**: 0x50 (7-bit)
- **Write protect**: None on this bus (writable)

There is a SECOND AT24C02 on PB6/PB14 (via PB15 level shifter) — all zeros,
write-protected. That chip is likely for lathe variants (shared HMI PCB).

## Memory Map

Sources: R2P06K disassembly (EEPROM functions at 0x80181ac/0x8018216/0x8018132),
motor_test EEPC dump, and cross-reference with MCB REGSCAN values.

### Header / Magic (0x00-0x03)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x00 | 1 | 0xFF | Padding |
| 0x01 | 1 | 0xD3 | Checksum/CRC high byte |
| 0x02 | 1 | 0x7C | **Settings magic** — firmware checks `cmp r0, #0x7C` at 0x8017c7a |
| 0x03 | 1 | 0xFF | Padding |

### Model / Firmware Version String (0x04-0x0F)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x04-0x0F | 12 | "DRIL5.20D_DP" | Product model + firmware version |

"DRIL" = Drill, "5.20D" = firmware version, "_DP" = Drill Press variant.
Read at boot via 0x80181d2 and compared to firmware constants for version matching.

### Depth Sensor Calibration (0x16-0x26)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x16-0x17 | 2 | 0xC1A9 | Depth cal value A (primary copy) |
| 0x18 | 1 | 0xB1 | Depth cal param |
| 0x19 | 1 | 0x00 | Depth cal param |
| 0x1A-0x1B | 2 | 0x3F12 | Depth cal value B — depth zero/offset (primary) |
| 0x1C-0x1D | 2 | 0x4E12 | Depth cal value C — depth scale (primary) |
| 0x1E-0x1F | 2 | 0xC1A9 | Depth cal value A (redundant copy) |
| 0x20-0x22 | 3 | 0xFF | Padding |
| 0x23-0x24 | 2 | 0x3F12 | Depth cal value B (redundant copy) |
| 0x25-0x26 | 2 | 0x4E12 | Depth cal value C (redundant copy) |

Redundant copies for error detection. Service menu → Height Sensor reads/writes these.
Default values from disassembly: A=30-ish, B=35-ish. Verify with service menu display.

### Depth Stop Settings (0x28-0x29)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x28 | 1 | 0x00 | Depth stop value (zeroed = no stop set) |
| 0x29 | 1 | 0x00 | Depth stop value |

### Motor / Speed Settings (0x30-0x3F)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x30-0x31 | 2 | 0x0064 | Max speed RPM / 10? Or CL default (100) |
| 0x32-0x33 | 2 | 0x00FA | Stored spindle speed / AC voltage threshold (250) |
| 0x34-0x39 | 6 | 0xFF | Unused |
| 0x3A | 1 | 0x02 | Stored flag (originally thought = direction; NOT loaded at boot — direction always defaults to FWD) |
| 0x3B | 1 | 0xFF | Unused |
| 0x3C | 1 | 0x00 | Direction/mode flag |
| 0x3D-0x3F | 3 | 0xFF | Unused |

### Configuration (0x40-0x4F)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x40-0x41 | 2 | 0x0032 | Feed override / speed factor (50) |
| 0x42-0x43 | 2 | 0xFF | Unused |
| 0x44-0x45 | 2 | 0x0BB8 | Max speed RPM (3000) — big-endian |
| 0x46-0x47 | 2 | 0xFF | Unused |
| **0x48** | **2** | **0xEEA0** | **Feature flags** (16-bit — see below) |
| 0x4A-0x4F | 6 | 0xFF | Unused |

### Feature Flags (0x48-0x49) — 16-bit packed bitfield
| Bit | Mask | Our Value | Description (from disassembly call sites) |
|-----|------|-----------|-------------------------------------------|
| 0 | 0x01 | 0 | Reserved |
| 1 | 0x02 | 1 | Forward direction enable |
| 2 | 0x04 | 1 | Reserved |
| 3 | 0x08 | 1 | **Unlock Func** — reveals AC Tapping in config menu |
| 4 | 0x10 | 0 | Reserved |
| 5 | 0x20 | 1 | Tapping mode enabled |
| 6 | 0x40 | 1 | Metric units |
| 7 | 0x80 | 1 | Depth stop enabled |
| 8 | 0x100 | 0 | Auto-reverse flag |
| 9 | 0x200 | 1 | Speed display on |
| 11 | 0x800 | 0 | Jog mode |
| 12 | 0x1000 | 0 | Soft-start flag |
| 13 | 0x2000 | 0 | Panel lock |
| 14 | 0x4000 | 0 | Beeper enable |

Our value: 0xEEA0 = flags byte 0xEE (11101110) + 0xA0 (10100000).
Byte 0x48=0xEE: bits 1,2,3,5,6,7 set. Byte 0x49=0xA0: bits 13,15 or 5,7 of high byte.

### F-Key Function Assignments (0x50-0x5B) — CORRECTED 2026-05-26
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x50 | 1 | 1 | F1 function (1 = Fav. Speed) |
| 0x52 | 1 | 2 | F2 function |
| 0x54 | 1 | 3 | F3 function |
| 0x56 | 1 | 4 | F4 function |
| 0x58-0x59 | 2 | 0xFF | Unused |
| 0x5A | 1 | 0 | Motion parameter |
| 0x5B | 1 | 4 | Number of active F-key functions |

**Per-F-key function options** (handler 0x8011eaa):
- 0: "-Do Not Use-"
- 1: "Fav. Speed" (toggles between 2 favorites)
- 2: "+/- Run Speed" (nudge)
- 3: "Change UNITS"
- 4: "Menu Shortcut"
- 5: "Lock Drill"

Default config: F1=1, F2=2, F3=3, F4=4. These are NOT preset slot indices —
each F-key has its own configurable role. Odd bytes 0x51/0x53/0x55/0x57 are
0xFF padding.

**Fav.Speed pairing** (when F-key is in Fav.Speed mode, each press toggles
between two presets):
- F1 ↔ EEPROM 0x64 (#1) / 0x74 (#5)
- F2 ↔ EEPROM 0x68 (#2) / 0x78 (#6)
- F3 ↔ EEPROM 0x6C (#3) / 0x7C (#7)
- F4 ↔ EEPROM 0x70 (#4) / 0x80 (#8)

### Tapping / Display Settings (0x60-0x63)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x60 | 1 | 25 (0x19) | Tapping speed preset (from disassembly) |
| 0x61 | 1 | 0xFF | Unused |
| 0x62 | 1 | 0 | Tapping direction flag |
| 0x63 | 1 | 0xFF | Unused |

### Speed Presets (0x64-0x83) — 8 × 16-bit big-endian, 4-byte stride
| Offset | Value | Description |
|--------|-------|-------------|
| 0x64 | 250 RPM | Preset 1 (low speed) |
| 0x68 | 900 RPM | Preset 2 (default SV) |
| 0x6C | 1600 RPM | Preset 3 |
| 0x70 | 3000 RPM | Preset 4 (max speed) |
| 0x74 | 500 RPM | Preset 5 |
| 0x78 | 1200 RPM | Preset 6 |
| 0x7C | 2000 RPM | Preset 7 |
| 0x80 | 2500 RPM | Preset 8 |

4-byte stride: 2 bytes value + 2 bytes 0xFF padding.
These are the user-configurable F1-F4 speed presets on the Voyager LCD.

### Display / Mode Flags (0x84-0xAF)
| Offset | Size | Dump | Description |
|--------|------|------|-------------|
| 0x84 | 1 | 1 | Display brightness / backlight mode (OEM; **unused by this firmware** — the LCD has no backlight-control pin, see below) |
| 0x85-0x93 | 15 | 0xFF | Unused |
| 0x94 | 1 | 0xFF | **Alt flag byte** — direction-lock / gear flags (via 0x8017c4c) |
| 0x95 | 1 | 0xFE | Unknown |
| 0x96-0x97 | 2 | 0xFF | Unused |
| 0x98 | 1 | 0x29 (41) | **Display/unit mode** — bits: 0-2=speed unit multiplier, 3=load bar, 4-6=display mode, 7=metric |
| 0x99-0x9F | 7 | 0xFF | Unused |
| 0xA0 | 1 | 0xFF | User config byte A |
| 0xA2 | 1 | 0xFF | User config byte B |
| 0xA4 | 1 | 0x00 | **Language ID** (1=DE, 2=FR, 3=EN; 0/other = default EN) — loaded at boot to RAM cache at *0x0801C654 |
| 0xA5-0xA7 | 3 | 0xFF | Unused |
| 0xA8 | 1 | 0x47 (71) | Saved MCB calibration byte (compared & re-written if changed) |

### Operational Data Cache (0xB0-0xFF) — **NOT unused**

Earlier assumption that this region was free for custom use was WRONG. The original
firmware writes operational cache here on first use. Observed after running original
firmware briefly (2026-05-26):

```
0xB0: CF 01 64 00 32 00 D0 07 28 23 A3 70 90 01 E8 03
0xC0: 4B 00 32 01 01 5A 03 01 01 02 3C 00 00 00 00 00
0xD0: 00 06 03 0B DC 05 00 28 07 FF FF FF AD DE 01 00
0xE0: 00 00 00 00 00 82 00 00 00 00 00 40 00 00 00 00
0xF0: 55 4E 4B 4E 4F 57 4E 00 A3 91 00 00 FF FF FF FF
```

**Identified fields (partial):**
- 0xBA-0xBB: 0x70A3 = **28835** — matches FAQ's "Ir Gain" value exactly
- 0xDC-0xDF: 0xAD DE 01 00 = **magic number 0x0001DEAD** (sentinel?)
- 0xF0-0xF7: ASCII **"UNKNOWN\0"** (model/serial placeholder)

**Implication for custom firmware:**
- If we want to preserve compatibility with original firmware, don't overwrite
  this region.
- If we don't care, 80 bytes are still available — but a factory reset from
  the original firmware would rewrite them.

## Custom Firmware Layout, as built (v1, 2026-08-30)

The 80 bytes at `0xB0-0xFF` are split between two records, and
`include/eeprom_layout.h` asserts the split at compile time:

| Range | Size | Contents |
|-------|------|----------|
| `0xB0-0xEC` | 61 | `eeprom_custom_t` — settings block, layout version 1 |
| `0xED-0xFF` | 19 | `crash_dump_t` — post-mortem record |

**Why the assertions exist.** Up to v0.1.0 these two overlapped by 13 bytes:
the settings struct had grown to 57 bytes at `0xB0` (reaching `0xE8`) while
`crash_dump.c` wrote a 36-byte record at `0xDC`. The overlap covered the
settings block's own checksum, so **every crash dump silently corrupted the
settings**, and the next boot failed validation and overwrote the block with
factory defaults. Neither this document, nor the header comment, nor
`crash_dump.c`'s comment matched the struct that was actually compiled — three
disagreeing authorities and no check. There is one authority now, and adding a
field to either record is a build error rather than a corruption.

**What the settings block does and does not hold.** 61 bytes against a 176-byte
`settings_t`, so it is a subset: motor tuning, all four jam / belt-break
detectors, depth mode/target/offset/action, tapping triggers and timings, step
drill, power output and the temperature trip — everything that changes how the
machine behaves mid-cut, so that a SAVE while the spindle is turning is both
safe and complete. The remainder (interface settings, and the material / bit
type / diameter used by CalcRPM) is mirrored to the last flash page by any SAVE
made while the motor is stopped, and `settings_init()` layers flash, then OEM,
then this block. See `include/settings_pack.h` and `test/test_settings_pack`.

**Magics and versions.** `EE_CUSTOM_MAGIC_VALUE` is `0xC1` and the layout
version is 1; the flash mirror uses `SETTINGS_MAGIC` "NOV1" and
`SETTINGS_VERSION` 1. Both counters were reset to 1 for the first public
release, and both magics were CHANGED at the same time — resetting a version
downward re-uses numbers already burned on pre-release layouts, and a rejected
blob is deliberately left on the chip, so a future bump back to 3 would
otherwise accept a stale v3 block with magic, version and checksum all
matching. A new magic makes every pre-release block unmatchable for good.

There is no migration by design: a blob of the wrong magic or version is
treated as absent, left intact on the chip, and defaults are used until the
operator saves. See the note in `settings_init()`.

## Custom Firmware Usage Plan

Our `settings_t` struct is ~220-240 bytes. Options:
1. **Use 0xB0-0xFF** (80 bytes) for critical custom settings only
2. **Overwrite Teknatool layout** entirely with our settings struct
3. **Keep Teknatool data** and store our settings in MCU flash (current approach)

Recommendation: Keep Teknatool data intact for now (allows reflashing original FW
without losing calibration). Use MCU flash for our settings. If we need EEPROM,
use 0xB0-0xFF which is guaranteed unused.

## Backup

Full hex dump: `EEPROM_BACKUP.txt` (same directory)

# Restorable Commands from Backup

Analysis of commands in `src/commands_debug.c.backup_full` that could be restored.

---

## ALREADY RESTORED ✓

- `cmd_beep` - Audio feedback test
- `cmd_buzz` - Direct buzzer hardware test
- `cmd_testcgram` - CGRAM custom character test
- `cmd_testlcd` - Comprehensive LCD capability test

---

## HIGH VALUE - RECOMMEND RESTORING

### Load Monitoring & Debugging (for testing new load triggers)

**cmd_loadmon** - Monitor motor load continuously
```c
// Displays motor_load from g_state (KR%) over time
// Useful for: Tuning load_increase_threshold
// Helps: Understand baseline vs loaded behavior
```

**cmd_loadbase** - Baseline load learning simulation
```c
// Simulates pre-tapping baseline learning phase
// Displays learned baseline over 2 seconds
// Useful for: Understanding load sensing calibration
```

**cmd_loadsense** - Test load sensing logic
```c
// Tests load spike detection with current settings
// Useful for: Verifying load_increase trigger
```

**Value:** Critical for tuning new Load Increase/Slip triggers!

---

### Protocol Discovery & Analysis

**cmd_scan** - Scan for working motor commands
```c
// Tests array of query commands (GF, SV, GV, UP, SA, etc.)
// Discovers which motor controller commands respond
// Useful for: Motor protocol reverse engineering
```

**cmd_listen** - Listen to motor UART for 5 seconds
```c
// Passively monitors motor traffic
// Shows unsolicited motor messages
// Useful for: Understanding motor controller behavior
```

**cmd_gscan** - Grouped command scan
```c
// Scans commands by groups
// More organized than individual scanning
```

**Value:** Useful for motor protocol exploration

---

### LCD Graphics Testing (for ST7920 reverse engineering)

**cmd_testallicons** - Test all icon positions
```c
// Tests icon drawing at all positions
// Verifies graphics mode coordinate system
// Useful for: Understanding ST7920 layout
```

**cmd_testicons** - Icon drawing tests
```c
// Various icon drawing tests
// Tests graphics mode capabilities
```

**cmd_compare3637** - Compare 0x36 vs 0x37 graphics modes
```c
// Tests difference between graphics mode commands
// Discovered: 0x37 gives full 128x64 access!
// Useful for: Graphics mode research
```

**cmd_testgfxv2** - Graphics v2 mode tests
```c
// Tests alternative graphics implementations
// Useful for: Graphics optimization research
```

**cmd_draw8boxes** - Draw 8 bordered boxes
```c
// Complement to draw8icons
// Tests box drawing primitives
```

**cmd_drawgrid** - Draw coordinate grid
```c
// Visual coordinate system reference
// Helps understand X/Y addressing
```

**Value:** Useful for LCD research, less critical for operation

---

### Coordinate Mapping (ST7920 address mapping)

**cmd_mapcoord** - Map single coordinate
```c
// Tests coordinate to address mapping
// Useful for: Understanding DDRAM addressing
```

**cmd_mapfull** - Full coordinate mapping
```c
// Maps entire display address space
// Discovers non-standard addressing
```

**cmd_testcoords** - Test coordinate system
```c
// Comprehensive coordinate tests
// Verifies mapping functions
```

**Value:** Useful for graphics development

---

## MEDIUM VALUE - CONSIDER RESTORING

### Hardware Testing

**cmd_adc** - Read single ADC channel
```c
// Read specific ADC channel value
// Note: cmd_adcmon might already cover this
```

**cmd_adcall** - Read all ADC channels
```c
// Scan all 16 ADC channels
// Useful for: Hardware exploration
```

**cmd_i2c** - I2C bus scan
```c
// Scan I2C bus for devices
// Useful for: Checking for EEPROM, sensors
```

**Value:** Moderate - useful for hardware debugging

---

### Motor Testing

**cmd_testcl** - Test CL (current limit) command
```c
// Tests motor power limit command
// Cycles through 20/50/70/100%
```

**cmd_testload** - Load detection tests
```c
// Tests load threshold detection
// Useful for: Tuning load triggers
```

**Value:** Useful for motor testing

---

## LOW VALUE - NOT RECOMMENDED

### LCD Implementation Details

- `cmd_testgfxclear` - Graphics clear implementation test
- `cmd_testgfxmap` - Graphics mapping test
- `cmd_testlowerhalf` - Lower half Y address test
- `cmd_testupperlower` - Upper/lower half tests
- `cmd_testx16` - X coordinate 16-bit tests
- `cmd_findlower` - Find lower half addresses
- `cmd_trylower` - Try lower half access
- `cmd_verifyclear` - Verify graphics clear
- `cmd_scanylower` - Scan Y lower addresses

**Why skip:** Very specialized, used during initial ST7920 research
**Status:** Research complete, findings documented

### Debug/Development

- `cmd_qq` - Quick query (probably redundant)
- `cmd_setdbg` - Set debug level (might not be implemented)
- `cmd_test` - Generic test (unclear purpose)

**Why skip:** Unclear value, may have dependencies

---

## RESTORATION PRIORITY

### Tier 1: CRITICAL (restore now)
- ✓ cmd_loadmon - Essential for load trigger tuning
- ✓ cmd_loadbase - Essential for baseline understanding
- ✓ cmd_loadsense - Test load sensing

### Tier 2: USEFUL (restore if needed)
- cmd_scan - Motor protocol discovery
- cmd_listen - Motor traffic monitoring
- cmd_testallicons - Icon position testing
- cmd_adc / cmd_adcall - ADC debugging
- cmd_i2c - I2C device scan

### Tier 3: RESEARCH (restore for deep dives)
- cmd_compare3637 - Graphics mode research
- cmd_testgfxv2 - Alternative graphics
- cmd_mapcoord / cmd_mapfull - Coordinate research

### Tier 4: SKIP (not needed)
- Most test* commands for LCD internals
- Specialized coordinate/mapping tests
- Debug commands without clear purpose

---

## RECOMMENDATION

**Restore immediately:**
1. Load monitoring commands (loadmon, loadbase, loadsense)
   - Critical for tuning new load triggers
   - Help understand KR baselines and spikes

2. Protocol commands (scan, listen)
   - Useful for motor controller exploration
   - Help validate trigger behavior

**Restore if needed:**
- Graphics commands if doing LCD research
- Hardware commands if debugging sensors

**Skip:**
- Specialized LCD internal tests (research complete)
- Unclear debug commands

---

## ESTIMATED EFFORT

**Tier 1 (3 commands):** ~30 minutes
- Extract from backup
- Add declarations
- Register in command table
- Test functionality

**Tier 2 (6 commands):** ~1 hour
**Tier 3 (5 commands):** ~1 hour

**Total for all useful commands:** ~2.5 hours

---

## NEXT STEPS

1. Restore Tier 1 (load monitoring) - high value for trigger tuning
2. Test load triggers with monitoring commands
3. Restore Tier 2 as needed for development
4. Skip Tier 4 (research already complete)

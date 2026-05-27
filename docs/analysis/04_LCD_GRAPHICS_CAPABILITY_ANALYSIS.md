# LCD Graphics Capability Analysis
**Version:** v2.2-final
**Date:** 2026-01-26
**Finding:** Original firmware may support pixel graphics beyond character mode

## Discovery

**User Report:** "pixel graphic is possible on the original firmware"

**Current Implementation:** Character-only mode (16x4 text via HD44780 interface)

**Question:** What graphics capabilities exist that we're not using?

---

## 1. Display Hardware Investigation

### What We Know (From Reverse Engineering)

**Hardware Connections:**
- **Data:** PA0-PA7 (8-bit parallel bus)
- **Control:** PB0 (RS), PB1 (RW), PB2 (E)
- **Interface:** HD44780-compatible
- **Size:** 16 columns × 4 rows

**Documented as:** "16x4 character LCD, HD44780-compatible"

### SPI2 Configuration Mystery

**From ORIGINAL_FIRMWARE_DEEP_ANALYSIS.md:**
```
SPI2 IRQ Handler exists (0x08005191)
Purpose: LCD display updates via SPI2
- Writes to SPI2_DR (0x4000380C)
- Checks SPI2_SR for TXE flag
Theory: SPI2 used for precise LCD timing or backlight PWM
```

**Pins:**
- PB12: Output push-pull (SPI2 CS? or LCD backlight?)
- PB13: AF push-pull (SPI2 CLK)
- PB15: AF push-pull (SPI2 MOSI)

**This suggests the display might be MORE than just HD44780!**

---

## 2. Possible Display Types

### Theory 1: ST7920-Based Display (Most Likely)

**ST7920 Controller:**
- Marketed as "12864" (128×64 pixels)
- **Dual mode:**
  - Character mode: 16×4 text (HD44780-compatible via parallel)
  - Graphics mode: 128×64 pixels (via SPI or parallel)
- Common in industrial equipment
- Backwards compatible with HD44780

**Evidence:**
- SPI2 configuration in original firmware
- HD44780-compatible character mode (what we use)
- User reports pixel graphics capability

**Resolution in Graphics Mode:**
- **128 × 64 pixels** total
- **16 × 4 character equivalent** (each char = 8×16 pixels in Chinese font mode)
- OR **16 × 4 with 5×8 ASCII** + graphics overlay

### Theory 2: HD44780 with CGRAM (Custom Characters)

**HD44780 CGRAM Capability:**
- 8 custom characters (addresses 0x00-0x07)
- Each character: 5×8 pixels programmable
- Total: **40×8 = 320 programmable pixels**

**What's Possible:**
- Icons (arrows, symbols, battery indicators)
- Simple bar graphs
- Custom fonts
- Animated characters

**Limitations:**
- Only 8 custom characters at a time
- Must redefine to change graphics
- Low resolution (5×8 per character)

### Theory 3: Hybrid Implementation

**Combination of both:**
- Character mode for text (what we use)
- CGRAM for icons/symbols (what original firmware uses)
- Possibly graphics mode for advanced displays (not confirmed)

---

## 3. Evidence Analysis

### SPI2 Usage in Original Firmware

**If display is ST7920 with graphics mode:**
- SPI2 would send pixel data faster than parallel
- Graphics mode requires different initialization sequence
- Would explain why SPI2 IRQ handler exists

**Commands to check:**
```
lcd_cmd(0x36);  // Extended instruction set enable (ST7920)
lcd_cmd(0x34);  // Graphics mode ON
lcd_cmd(0x36);  // Graphics display ON
```

**If we see these in original firmware → Confirms ST7920 graphics capability**

### What to Look For

**In original firmware binary (if available):**
1. Search for bytes: `0x36, 0x34, 0x36` (ST7920 graphics mode sequence)
2. Search for SPI2 transmit patterns (pixel data)
3. Check if CGRAM is written (addresses 0x40-0x7F)
4. Look for bitmap data structures

**In our current code:**
- ❌ No CGRAM usage (we don't define custom characters)
- ❌ No SPI2 initialization
- ❌ No graphics mode commands
- ✅ Only basic HD44780 text commands

---

## 4. Capabilities We're Missing

### If ST7920 Graphics Mode Exists

**Current (Character Mode):**
```
16 × 4 text = 64 characters
5×8 font = 80 × 32 effective pixels (text only)
```

**Possible (Graphics Mode):**
```
128 × 64 pixels = 8,192 programmable pixels
Full bitmap control
Can draw:
  • Lines, boxes, circles
  • Bar graphs, gauges
  • Icons, symbols
  • Waveforms
  • Custom UI elements
```

**Mixed Mode:**
- Text in some areas
- Graphics in others
- Best of both worlds

### If Just CGRAM Custom Characters

**Current:** No custom characters defined

**Possible:**
- 8 custom 5×8 icons at a time
- Examples:
  - ▶ Play/pause icons
  - ↑↓ Direction arrows
  - ⚠ Warning symbol
  - ✓ Checkmark
  - Battery level bars
  - Signal strength
  - Thermometer icon
  - Rotating busy indicator

**Impact:** Better visual feedback without changing hardware

---

## 5. Investigation Plan

### Step 1: Confirm Display Hardware

**Physical inspection:**
1. Read LCD controller chip markings
2. Look for: "ST7920", "12864", "UC1701", etc.
3. Check datasheet based on chip ID

### Step 2: Test CGRAM (5 minutes)

**Add to lcd.c:**
```c
void lcd_create_char(uint8_t location, const uint8_t* data) {
    location &= 0x07;  // Only 8 locations (0-7)
    lcd_cmd(0x40 | (location << 3));  // Set CGRAM address
    for (int i = 0; i < 8; i++) {
        lcd_data(data[i]);  // Write 8 bytes of pixel data
    }
    lcd_cmd(0x80);  // Return to DDRAM
}

// Define a heart icon
uint8_t heart[8] = {
    0b00000,
    0b01010,
    0b11111,
    0b11111,
    0b01110,
    0b00100,
    0b00000,
    0b00000
};

// Create and display
lcd_create_char(0, heart);  // Store at position 0
lcd_data(0x00);  // Display custom char 0 (heart)
```

**Test:** If heart appears → CGRAM works, can create custom graphics

### Step 3: Test ST7920 Graphics Mode (10 minutes)

**Try graphics mode sequence:**
```c
void lcd_test_graphics_mode(void) {
    // Try ST7920 extended instruction set
    lcd_cmd(0x30);  // Basic instruction set
    lcd_delay_ms(1);
    lcd_cmd(0x34);  // Extended instruction set
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);

    // Try to set graphics position and write pixel data
    lcd_cmd(0x80);  // Y address
    lcd_cmd(0x80);  // X address
    lcd_data(0xFF);  // Write pixel data (all white)
    lcd_data(0xFF);

    // Return to text mode
    lcd_cmd(0x30);
    lcd_delay_ms(1);
}
```

**Test:** If pixels appear → Graphics mode works!

### Step 4: Analyze Original Firmware Binary

**What to search for:**
```bash
# Look for ST7920 graphics mode initialization
strings original_firmware.bin | grep -i "graph\|ST7920\|extended"

# Look for CGRAM writes (0x40-0x7F commands)
hexdump -C original_firmware.bin | grep "40 [0-7][0-9a-f]"

# Look for SPI2 pixel data patterns
# Regular patterns of 16 bytes = potential graphics data
```

---

## 6. Potential Graphics Features

### If CGRAM Only (Conservative)

**8 Custom Characters Available:**
1. ▶ Play icon
2. ⏸ Pause icon
3. ⏹ Stop icon
4. ↑ Up arrow
5. ↓ Down arrow
6. ⚠ Warning symbol
7. ✓ Check/OK icon
8. ◀ Reverse icon

**UI Improvements:**
```
Row 0: [▶] 1200 RPM  FWD
Row 1: Depth: 5.2mm
Row 2: [✓] Ready
Row 3: Tap Mode: OFF
```

**Implementation:** 1-2 hours

### If ST7920 Graphics Mode (Ambitious)

**Full 128×64 Pixel Display:**

**Possible UI:**
```
┌────────────────────────────────┐
│  NOVA VOYAGER    [1200 RPM]    │ ← Text + graphics
│  ▄▄▄▄▄▄▄▄▄▄▄▄░░░░  75%         │ ← Bar graph
│  Depth: ↓ 5.2mm  Temp: 45°C    │ ← Icons + text
│  Mode: PECK  ▶                 │ ← Status icons
└────────────────────────────────┘
```

**Features:**
- Real-time bar graphs (RPM, load, depth)
- Icons for modes
- Graphical menus
- Waveforms (speed over time)
- Animated indicators

**Implementation:** 8-12 hours (requires graphics library)

---

## 7. Risk Assessment

### Risks of Adding Graphics

**Technical Risks:**
1. **Unknown hardware** - Need to confirm display type
2. **Timing sensitive** - Graphics mode may require different timing
3. **Memory usage** - Frame buffer for 128×64 = 1KB RAM (8% of total)
4. **CPU usage** - Redrawing graphics takes time
5. **SPI conflicts** - If SPI2 used, need careful initialization

**Development Risks:**
1. **Scope creep** - Graphics is a big feature
2. **Testing burden** - Need to test all graphics rendering
3. **Maintenance** - Graphics code is complex
4. **Documentation** - Need to document graphics API

### Rewards

**User Experience:**
- Much better visual feedback
- Professional appearance
- Real-time graphs (very useful for drilling)
- Clearer status indication

**Capability:**
- Display more information simultaneously
- Animated indicators (busy, progress)
- Graphical menus (easier navigation)

---

## 8. Recommended Approach

### Phase 1: Investigation (30 minutes)

1. **Physical inspection:**
   - Read LCD controller chip marking
   - Confirm: ST7920, HD44780, or other?

2. **CGRAM test:**
   - Try creating 1 custom character
   - If works → Can add icons easily

3. **Graphics mode test:**
   - Try ST7920 graphics sequence
   - If works → Full pixel graphics possible

### Phase 2: Decision Point

**If CGRAM works:**
- ✅ Add 8 useful icons (low effort, high value)
- Implementation: 1-2 hours
- Risk: Very low
- **Recommended: YES**

**If ST7920 graphics works:**
- ⚠️ Consider full graphics mode
- Implementation: 8-12 hours
- Risk: Medium
- **Recommended: Maybe (depends on use case)**

### Phase 3: Implementation (If Proceeding)

**Minimal (CGRAM Icons):**
```c
// Add to lcd.h
void lcd_create_char(uint8_t location, const uint8_t* data);
void lcd_load_icons(void);  // Load standard icon set

// Define icons in lcd.c
#define ICON_PLAY    0
#define ICON_STOP    1
#define ICON_WARNING 2
// ... etc

// Use in display.c
lcd_data(ICON_PLAY);  // Show play icon
```

**Full (ST7920 Graphics):**
```c
// Add to lcd.h
void lcd_graphics_mode(bool enable);
void lcd_set_pixel(uint8_t x, uint8_t y, bool on);
void lcd_draw_line(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
void lcd_draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void lcd_draw_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t percent);
void lcd_update_framebuffer(void);  // Push to display
```

---

## 9. Comparison: Text vs Graphics

### Current Text-Only UI

**Strengths:**
- ✅ Simple, reliable
- ✅ Low CPU/RAM usage
- ✅ Easy to update
- ✅ Works on any HD44780 display

**Limitations:**
- ❌ No visual indicators (icons, graphs)
- ❌ Limited information density
- ❌ Static appearance
- ❌ No real-time visualization

### Potential Graphics UI

**Strengths:**
- ✅ Professional appearance
- ✅ Real-time bar graphs (RPM, load, depth)
- ✅ Icons for status/mode
- ✅ More information visible simultaneously

**Limitations:**
- ❌ Requires 1KB RAM (frame buffer)
- ❌ Higher CPU usage (redraw graphics)
- ❌ More complex code (graphics primitives)
- ❌ Hardware dependency (ST7920 required)

---

## 10. Recommendations

### Immediate Action: **INVESTIGATE**

**30-minute test to confirm capabilities:**

1. **Test CGRAM (Custom Characters):**
```c
// Add to commands_debug.c
void cmd_testicon(void) {
    // Heart icon test
    uint8_t heart[8] = {0b00000, 0b01010, 0b11111, 0b11111,
                        0b01110, 0b00100, 0b00000, 0b00000};

    // Program CGRAM location 0
    lcd_cmd(0x40);  // CGRAM address 0
    for (int i = 0; i < 8; i++) {
        lcd_data(heart[i]);
    }

    // Display it
    lcd_cmd(0x80);  // Return to DDRAM
    lcd_data(0x00); // Display custom char 0

    uart_puts("Custom character test - check LCD\r\n");
}
```

Run `TESTICON` command → If heart appears, CGRAM works ✅

2. **Test ST7920 Graphics Mode:**
```c
void cmd_testgfx(void) {
    // Try extended instruction set
    lcd_cmd(0x34);  // Extended
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);

    // Try to write pixel data
    lcd_cmd(0x80);  // Set Y
    lcd_cmd(0x80);  // Set X
    lcd_data(0xFF); // All pixels on
    lcd_data(0xFF);

    uart_puts("Graphics mode test - check LCD for pixels\r\n");

    // Return to text mode
    lcd_cmd(0x30);
}
```

Run `TESTGFX` command → If pixels appear, ST7920 graphics works ✅

### Decision Tree

```
                    Investigate (30 min)
                           ↓
                ┌──────────┴──────────┐
                ↓                      ↓
           CGRAM works?          Graphics mode works?
                ↓                      ↓
           Add icons          Consider full graphics
           (1-2 hours)        (8-12 hours)
           LOW RISK           MEDIUM RISK
           ✅ RECOMMENDED     ⚠️ EVALUATE
```

---

## 11. Proposed: CGRAM Icon Implementation (Quick Win)

### Effort: 1-2 hours
### Value: MEDIUM-HIGH (better UX)
### Risk: LOW (doesn't break existing functionality)

**Icon Set (8 characters):**
1. **▶** Play/Forward
2. **◀** Reverse
3. **⏹** Stop
4. **⚠** Warning
5. **✓** OK/Ready
6. **↑** Up/Ascending
7. **↓** Down/Descending
8. **●** Active indicator (can animate by redefining)

**Enhanced UI Example:**
```
Before (text only):
  1200 RPM  FWD
  Depth: 5.2mm
  Ready
  Tap Mode: OFF

After (with icons):
  ▶ 1200 RPM  →
  ↓ Depth: 5.2mm
  ✓ Ready
  Tap Mode: OFF
```

**Implementation:**
```c
// In lcd.c
static const uint8_t icon_play[8]    = {0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10, 0x00};
static const uint8_t icon_stop[8]    = {0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00};
static const uint8_t icon_warning[8] = {0x04, 0x0E, 0x0E, 0x0E, 0x00, 0x04, 0x00, 0x00};
static const uint8_t icon_ok[8]      = {0x00, 0x01, 0x03, 0x16, 0x1C, 0x08, 0x00, 0x00};
// ... etc

void lcd_load_icons(void) {
    lcd_create_char(ICON_PLAY, icon_play);
    lcd_create_char(ICON_STOP, icon_stop);
    lcd_create_char(ICON_WARNING, icon_warning);
    lcd_create_char(ICON_OK, icon_ok);
    // ... load all 8
}

// In main.c boot sequence
lcd_load_icons();  // One-time load at startup
```

**Usage in display.c:**
```c
// Show play icon before RPM
lcd_data(ICON_PLAY);
lcd_print(" 1200 RPM");
```

---

## 12. Proposed: Full Graphics Investigation (If Ambitious)

### If ST7920 Confirmed

**Phase 1: Graphics Mode Proof of Concept (2-3 hours)**
1. Initialize ST7920 graphics mode
2. Draw simple test pattern (pixels, lines, box)
3. Verify SPI2 or parallel graphics works

**Phase 2: Graphics Library (6-8 hours)**
1. Frame buffer (1KB RAM for 128×64)
2. Pixel, line, rectangle primitives
3. Text rendering on graphics (5×8 font)
4. Update function (frame buffer → display)

**Phase 3: Enhanced UI (4-6 hours)**
1. Redesign UI with graphics elements
2. Real-time bar graphs (RPM, load)
3. Icons for all modes
4. Animated busy indicators

**Total Effort:** 12-17 hours

**Value:** High (professional UI) but significant effort

---

## 13. What We Should Do NOW

### My Recommendation: **START WITH CGRAM TEST**

**Why:**
1. **30 minutes to confirm** CGRAM works (very low effort)
2. **If it works:** Add 8 icons (1-2 hours, high value)
3. **Low risk:** Doesn't break existing text display
4. **Immediate benefit:** Better visual feedback

**Then:**
- **If satisfied:** Stop there (icons are enough)
- **If want more:** Investigate full ST7920 graphics mode

### Quick Test Code

**I can implement right now:**
```c
void cmd_testicon(void) {
    // Test CGRAM custom character creation
    uint8_t heart[8] = {0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00};

    lcd_cmd(0x40);  // CGRAM address 0
    for (int i = 0; i < 8; i++) {
        lcd_data(heart[i]);
    }
    lcd_cmd(0x80);  // Back to DDRAM
    lcd_set_cursor(0, 0);
    lcd_data(0x00); // Display custom char 0
    lcd_print(" <- Heart icon test");

    uart_puts("CGRAM test complete - check LCD for heart icon\r\n");
}
```

**Test:** Run `TESTICON` command and check if heart appears

---

## 14. Conclusions

### Analysis Summary

**Current State:**
- Using HD44780 character mode only
- No custom characters (CGRAM unused)
- No graphics mode (if available)
- **Missing potential display capabilities**

**Likely Capability:**
- **Minimum:** CGRAM custom characters (8 icons) - **99% certain**
- **Possible:** ST7920 graphics mode (128×64 pixels) - **60% likely**

**Evidence:**
- SPI2 configured in original firmware
- User reports pixel graphics capability
- Display likely ST7920 (common in industrial equipment)

### Recommended Actions

**Priority 1 (HIGH VALUE):**
✅ **Test CGRAM** (30 min) - Confirm custom characters work
✅ **Add 8 icons** (1-2 hours) - Immediate UX improvement

**Priority 2 (MEDIUM VALUE):**
⚠️ **Investigate ST7920** (2-3 hours) - Confirm graphics mode
⚠️ **Prototype graphics UI** (4-6 hours) - If graphics mode works

**Priority 3 (LOW VALUE):**
⏭️ **Full graphics implementation** (12-17 hours) - Only if really needed

### Next Steps

**I can help you:**
1. **Add CGRAM test command** (5 minutes) - See if custom chars work
2. **Add ST7920 graphics test** (10 minutes) - See if pixel mode works
3. **Implement icon set** (1-2 hours) - If CGRAM confirmed
4. **Full graphics library** (12-17 hours) - If ST7920 confirmed + you want it

**What would you like to do?**
- Test CGRAM capability first? (Quick, low risk)
- Full analysis of original firmware binary? (More thorough)
- Or something else?

---

**Finding:** We're likely only using 10-20% of the display's capabilities. Custom characters (CGRAM) are almost certainly available. Full pixel graphics (ST7920) are possible but need confirmation.

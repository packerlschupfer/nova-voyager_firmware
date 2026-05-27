# Debug Output Optimization Strategy

## Overview

The codebase contains 1,196 debug statements consuming 27.2 KB of flash. By using conditional compilation macros, we can eliminate this overhead in production builds.

## Implementation

### 1. Macro System (config.h)

```c
// Enable debug UART output (disabled in production builds)
#ifndef ENABLE_DEBUG_OUTPUT
  #ifdef NDEBUG
    #define ENABLE_DEBUG_OUTPUT 0  // Production: No debug output
  #else
    #define ENABLE_DEBUG_OUTPUT 1  // Debug: Enable all output
  #endif
#endif

#if ENABLE_DEBUG_OUTPUT
  #define DEBUG_PRINT(msg)        uart_puts(msg)
  #define DEBUG_PRINTC(ch)        uart_putc(ch)
  #define DEBUG_PRINTNUM(num)     print_num(num)
#else
  #define DEBUG_PRINT(msg)        ((void)0)
  #define DEBUG_PRINTC(ch)        ((void)0)
  #define DEBUG_PRINTNUM(num)     ((void)0)
#endif
```

### 2. Conversion Guidelines

**CONVERT to DEBUG_PRINT:**
- Event logging ("EVT: ZERO", "EVT: MENU", etc.)
- Status messages ("[TASK_MOTOR] UART TX timeout")
- Boot messages ("Cold boot - full MCB init")
- Non-critical warnings

**KEEP as uart_puts:**
- Safety-critical errors (COMM FAULT, OVERHEAT)
- User-facing error messages (shown on LCD)
- Fault conditions that require operator attention

### 3. Measured Impact

**Partial Conversion (Events only):**
- Flash before: 86,032 bytes
- Flash after: 85,272 bytes
- **Savings: 760 bytes (0.9%)**

**Projected Full Conversion:**
- Estimated savings: **18-22 KB (21-26%)**
- Based on analysis showing 1,196 debug statements

### 4. Build Configurations

**Production (nova_voyager):**
```ini
build_flags =
    -D NDEBUG              # Disables debug output
    -D BUILD_RELEASE=1
    -D USE_120MHZ=1
    -D LOG_LEVEL=1
    -Os                    # Optimize for size
```

**Debug (debug_120):**
```ini
build_flags =
    -D BUILD_DEBUG=1       # Enables debug output
    -D USE_120MHZ=1
    -D LOG_LEVEL=3
    -Og                    # Optimize for debugging
    -g                     # Debug symbols
```

### 5. Conversion Script

Use the following sed commands to convert files:

```bash
# Events file (all EVT messages)
sed -i 's/uart_puts("EVT:/DEBUG_PRINT("EVT:/g' src/events.c

# Task motor (selective - keep FAULT/ERROR)
sed -i 's/uart_puts("\[TASK_MOTOR\]/DEBUG_PRINT("[TASK_MOTOR]/g' src/task_motor.c
sed -i 's/uart_puts("Motor task started/DEBUG_PRINT("Motor task started/g' src/task_motor.c

# Tapping task
sed -i 's/uart_puts("\[TAPPING\]/DEBUG_PRINT("[TAPPING]/g' src/tapping.c

# UI task
sed -i 's/uart_puts("EVT:/DEBUG_PRINT("EVT:/g' src/task_ui.c
```

### 6. Safety-Critical Messages (NEVER CONVERT)

These must remain as `uart_puts()` for operator visibility:

```c
// Motor communication failure
uart_puts("COMM FAULT!\r\n");

// Temperature shutdown
uart_puts("EVT: OVERHEAT SHUTDOWN!\r\n");

// E-Stop events
uart_puts("EVT: E-STOP ");

// Guard events
uart_puts("EVT: GUARD OPENED - stopping motor + spindle hold!\r\n");
```

### 7. Verification

After conversion:

```bash
# Build production
pio run -e nova_voyager

# Verify strings are removed
strings .pio/build/nova_voyager/firmware.elf | grep "EVT:" | wc -l
# Should be 0 or very few (only critical ones)

# Check flash usage
pio run -e nova_voyager | grep Flash
# Should show significant reduction
```

### 8. Testing Checklist

- [ ] Production build compiles cleanly
- [ ] Debug build shows all messages
- [ ] Safety messages still visible in production
- [ ] E-Stop, Guard, START button functionality unchanged
- [ ] Motor control works correctly
- [ ] Flash usage reduced by 18-22 KB

## Current Status

**Phase 1 Complete:**
- ✅ Macro system implemented in config.h
- ✅ Partial conversion (events.c) - saved 760 bytes
- ✅ Production build verified

**Next Steps:**
- Convert task_motor.c debug output (est. 8-10 KB savings)
- Convert tapping.c debug output (est. 4-6 KB savings)
- Convert remaining files (est. 5-6 KB savings)
- Full regression testing

## Recommendations

1. **Do conversion incrementally** - one file at a time with testing
2. **Keep safety messages unconditional** - operator visibility is critical
3. **Test both builds** - verify debug build has output, production doesn't
4. **Document exceptions** - note any messages kept for specific reasons
5. **Use version control** - commit after each successful conversion

## Flash Budget

| Build Type | Current | After Conversion | Headroom |
|------------|---------|------------------|----------|
| Production | 86 KB   | ~64 KB (est.)    | 200 KB   |
| Debug      | N/A     | ~86 KB           | 178 KB   |

Production build will have excellent flash headroom for future features.

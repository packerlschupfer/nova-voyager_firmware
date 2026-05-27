# Nova Firmware Architecture Diagrams

**Version:** 2.0-complete
**Created:** 2026-01-26
**Format:** PlantUML (.puml)

This directory contains comprehensive PlantUML diagrams documenting the Nova Voyager firmware architecture after the v2.0 code cleanup and refactoring.

## Diagram Index

### 01. Module Architecture
**File:** `01_module_architecture.puml`
**Type:** Component Diagram
**Shows:**
- 7 firmware modules and their responsibilities
- Dependencies between modules
- Hardware layer interfaces (USART3, USART1, GPIO)
- Phase annotations (which phase created/modified each module)
- Unit test coverage indicators

**Key Insights:**
- Protocol layer (Phase 6) used by both motor.c and task_motor.c
- Feature modules (spindle_hold, jam, temperature) extracted from task_motor.c
- Clear separation: Application → Feature → Control → Protocol → Utility layers

### 02. FreeRTOS Task Communication
**File:** `02_freertos_tasks.puml`
**Type:** Sequence Diagram
**Shows:**
- 5 FreeRTOS tasks with priorities and stack sizes
- Queue communication (g_event_queue, g_motor_cmd_queue)
- Mutex protection (g_state_mutex, g_motor_mutex, g_uart_mutex)
- Task initialization sequence
- Runtime periodic operation
- Queue overflow handling (Phase 1.2)
- Watchdog refresh mechanism

**Key Insights:**
- Motor task highest priority (4) - 10Hz polling
- Tapping task (priority 3) - 20Hz state machine
- UI and Depth tasks (priority 2) - 50Hz
- Main task lowest priority (1) - 100Hz event processing
- Watchdog only refreshed if ALL tasks alive

### 03. Motor Protocol Sequence
**File:** `03_motor_protocol.puml`
**Type:** Sequence Diagram
**Shows:**
- QUERY packet format (read status/parameters)
- COMMAND packet format (set parameters)
- Protocol building flow through motor_protocol.c (Phase 6)
- Response parsing and validation
- Error handling (Phase 1.1 timeouts + Phase 7 diagnostics)
- Typical motor start sequence (CL→ST→SV)
- Status polling sequence (GF query)

**Key Insights:**
- Query format: 9 bytes, no checksum
- Command format: Variable length, XOR checksum from unit byte
- Checksum includes ETX (discovered via reverse engineering)
- All UART operations timeout-protected

### 04. Boot Sequence Flow
**File:** `04_boot_sequence.puml`
**Type:** Activity Diagram
**Shows:**
- Complete boot flow from power-on to ready state
- Boot type detection (COLD, SOFT, WATCHDOG, PIN)
- Peripheral initialization
- MCB initialization sequence (Phase 8 refactored)
- Wait for MCB ready (GF bit 3 clear)
- FreeRTOS scheduler start
- Task creation order

**Key Insights:**
- Soft boot skips splash for fast restart (OFF button)
- MCB needs 50ms power-up time
- Safety stop (RS×3) before any initialization
- GF bit 3 indicates MCB initialization complete (max 500ms wait)
- init_mcb_boot_sequence() encapsulates entire boot (Phase 8)

### 05. Motor State Machine
**File:** `05_motor_state_machine.puml`
**Type:** State Diagram
**Shows:**
- Motor states (STOPPED, RUNNING, ERROR)
- GF flag values for each state
- Spindle hold sub-state (Phase 2.1)
- Jam detection transitions (Phase 2.3)
- Temperature monitoring (Phase 2.2)
- Error states and recovery
- Hardware failsafe on UART timeout (Phase 1.1b)

**Key Insights:**
- GF=32/34 (forward), GF=436/438 (reverse)
- Spindle hold maintains position at 10-12% current limit
- Jam triggers at 90% load for 5 seconds
- Overheat shutdown at 80°C, warning at 60°C
- Hardware PD4 disable ensures motor always stops

### 06. Tapping Modes
**File:** `06_tapping_modes.puml`
**Type:** State Diagram
**Shows:**
- 6 tapping modes (OFF, PEDAL, SMART, DEPTH, LOAD, PECK)
- State transitions within each mode
- Trigger conditions (foot pedal, depth, load, time)
- CV burst pattern (discovered 2026-01-25)
- Queue stress during PECK mode (10ms timeout)

**Key Insights:**
- MODE 1 (PEDAL): User-controlled via foot pedal
- MODE 2 (SMART): Follows quill direction automatically
- MODE 3 (DEPTH): Reverses at target depth
- MODE 4 (LOAD): CV overshoot detection (through-hole detection)
- MODE 5 (PECK): Rapid timed cycles (stress tests queues)
- All modes use MOTOR_CMD macro with overflow protection

### 07. Error Handling Flow
**File:** `07_error_handling_flow.puml`
**Type:** Activity Diagram
**Shows:**
- UART timeout handling (Phase 1.1)
- Queue overflow handling (Phase 1.2)
- Motor stop failsafe (Phase 1.1b)
- Communication failure tracking (H5)
- Jam detection flow (Phase 2.3)
- Temperature monitoring flow (Phase 2.2)
- Diagnostics tracking integration (Phase 7)

**Key Insights:**
- Every error path logs + increments diagnostics counter
- Critical errors (motor stop timeout) use hardware failsafe
- System never hangs - always recovers within 100ms
- Graceful degradation (log + continue when possible)
- Emergency stop on queue full for motor commands

### 08. Module Dependencies
**File:** `08_module_dependencies.puml`
**Type:** Component Diagram
**Shows:**
- Detailed dependencies between all modules
- Function call relationships
- Data flow directions
- Phase annotations (when each module was created)
- Unit test coverage indicators
- Hardware access points

**Key Insights:**
- Protocol layer has no dependencies except utilities
- Diagnostics monitors queues but doesn't control them
- SpindleHold, Jam, Temperature all call back through Motor API
- Clear layering: Application → Feature → Control → Protocol → Utility

### 09. Before/After Transformation
**File:** `09_before_after_transformation.puml`
**Type:** Component Comparison
**Shows:**
- v1.0 architecture (2 monolithic files)
- v2.0 architecture (7 organized modules)
- Problems in original code
- Solutions in refactored code
- Metrics comparison
- New capabilities

**Key Insights:**
- task_motor.c reduced from 1,380 → 1,207 lines (-12.5%)
- motor.c reduced from 1,116 → 1,038 lines (-7.0%)
- 5 new modules created
- 23 unit tests added
- Comprehensive diagnostics system
- Cannot hang (was critical issue in v1.0)

### 10. Data Structures
**File:** `10_data_structures.puml`
**Type:** Class Diagram
**Shows:**
- shared_state_t (global state)
- motor_status_t (MCB status)
- diagnostics_t (system health counters)
- motor_cmd_t (queue message)
- event_type_t (UI events)
- jam_status_t (jam detection state)
- crash_dump_t (crash logging)
- settings_t (user configuration)
- Thread safety classifications

**Key Insights:**
- g_state protected by g_state_mutex (Phase 5)
- diagnostics_t has 19 counters tracking everything
- Queue messages are small (4-8 bytes)
- All structures use static allocation (no malloc)

### 11. Diagnostics System
**File:** `11_diagnostics_system.puml`
**Type:** Component Diagram
**Shows:**
- What diagnostics.c monitors (UART, queues, protocol, MCB, system)
- Diagnostic API functions (tracking calls)
- Reporting commands (STATS, ERRORS, PERF)
- Data flow from monitored components to counters
- Example output from each command

**Key Insights:**
- Tracks 19 different metrics
- 3 different views (full report, errors only, performance)
- Integrates with CRASHSHOW for context
- Non-intrusive monitoring (just increments counters)

### 12. Complete Data Flow
**File:** `12_data_flow_complete.puml`
**Type:** Sequence Diagram
**Shows:**
- End-to-end flow: User input → Motor action
- All 5 FreeRTOS tasks in action
- Queue communication paths
- Mutex protection points
- Serial console command handling
- Depth monitoring integration
- Tapping mode coordination
- Watchdog monitoring

**Key Insights:**
- Clean separation between tasks via queues
- Mutex protection at all shared state access
- Event-driven architecture
- All paths include error handling and diagnostics

## Viewing the Diagrams

### Method 1: PlantUML Online (Easiest)
1. Go to http://www.plantuml.com/plantuml/uml/
2. Copy/paste .puml file content
3. View rendered diagram

### Method 2: VS Code Extension
1. Install "PlantUML" extension
2. Open .puml file
3. Press Alt+D to preview

### Method 3: Command Line
```bash
# Install PlantUML
sudo apt install plantuml

# Generate PNG
plantuml docs/diagrams/*.puml

# Generates: *.png files
```

### Method 4: IntelliJ/CLion
1. Built-in PlantUML support
2. Right-click .puml → "Show Diagram"

## Diagram Usage

### For New Developers
Start with:
1. **09_before_after_transformation.puml** - See what was improved
2. **01_module_architecture.puml** - Understand module structure
3. **02_freertos_tasks.puml** - Learn task communication
4. **03_motor_protocol.puml** - Understand MCB protocol

### For Debugging
Use:
- **07_error_handling_flow.puml** - Understand error paths
- **11_diagnostics_system.puml** - Know what's monitored
- **12_data_flow_complete.puml** - Trace data through system

### For Feature Development
Reference:
- **01_module_architecture.puml** - Where to add code
- **08_module_dependencies.puml** - What dependencies exist
- **10_data_structures.puml** - What data structures to use

### For Testing
Check:
- **06_tapping_modes.puml** - Test all 6 modes
- **05_motor_state_machine.puml** - Test all state transitions
- **04_boot_sequence.puml** - Test cold/soft/watchdog boots

## Diagram Maintenance

When modifying firmware:
1. **Update affected diagrams** - Keep in sync with code
2. **Add phase annotations** - Document what changed
3. **Update metrics** - Keep statistics current
4. **Test implications** - Update test coverage notes

## Related Documentation

- `CLEANUP_SUMMARY.md` - Phases 1-5 implementation details
- `PHASES_6_8_SUMMARY.md` - Protocol and init improvements
- `FINAL_SUMMARY.md` - Complete technical overview
- `PROJECT_COMPLETE.md` - Executive summary

---

*PlantUML diagrams created as part of Nova Firmware v2.0 Code Cleanup*
*All diagrams reflect actual implementation and tested behavior*

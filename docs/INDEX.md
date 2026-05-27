# Nova Voyager Open Firmware — Documentation Index

## Quick Start

1. **Build:** `pio run -e nova_voyager` (or `nova_voyager_games` for games)
2. **Flash:** `./flash_firmware.sh quick` (or `custom` for first-time install)
3. **Serial:** 9600 baud on PA9/PA10, type `HELP` for command list
4. **Bench setup:** [`HARDWARE_SETUP.md`](HARDWARE_SETUP.md)

## Key Documents

| Document | What it covers |
|----------|---------------|
| [`../README.md`](../README.md) | Features, LCD layout, F-keys, architecture, safety |
| [`HARDWARE_SETUP.md`](HARDWARE_SETUP.md) | ST-Link wiring, Pi flash topology, serial console, logic analyzer |
| [`MOTOR_PROTOCOL.md`](MOTOR_PROTOCOL.md) | MCB UART protocol, command table, GF flags, fault codes, register dump |
| [`EEPROM_MAP.md`](EEPROM_MAP.md) | AT24C02 layout, OEM fields, custom block at 0xB0 |
| [`CGRAM_IMPLEMENTATION_GUIDE.md`](CGRAM_IMPLEMENTATION_GUIDE.md) | ST7920 16x16 CGRAM protocol (confirmed working) |

## Architecture Diagrams

PlantUML diagrams in [`diagrams/`](diagrams/):

| Diagram | Content |
|---------|---------|
| `01_module_architecture.puml` | 7 modules, dependencies |
| `02_freertos_tasks.puml` | 5 tasks, queues, mutexes |
| `03_motor_protocol.puml` | MCB packet formats |
| `04_boot_sequence.puml` | Power-on to ready |
| `05_motor_state_machine.puml` | Motor states, safety |
| `06_tapping_modes.puml` | 6 triggers, transitions |

View: paste into [plantuml.com](http://www.plantuml.com/plantuml/) or use VS Code PlantUML extension.

## Reverse Engineering Notes

These documents record the investigation process. Some conclusions have
been superseded by later findings — check dates and "SUPERSEDED" headers.

| Topic | Key documents |
|-------|--------------|
| Motor protocol | `MOTOR_PROTOCOL.md`, `MOTOR_PROTOCOL_ACTUAL_BEHAVIOR.md` |
| Original firmware | `ORIGINAL_FIRMWARE_DEEP_ANALYSIS.md`, `ORIGINAL_FIRMWARE_MOTOR_COMMANDS.md` |
| LCD / graphics | `GRAPHICS_INVESTIGATION_COMPLETE.md` (superseded), `CGRAM_IMPLEMENTATION_GUIDE.md` |
| Temperature | `TEMPERATURE_INVESTIGATION.md`, `THERMAL_POWER_MANAGEMENT.md` |
| GF flags | `GF_FLAG_BITS_REFERENCE.md`, `GF_FLAG_BITS_COMPLETE_ANALYSIS.md` |
| EEPROM | `EEPROM_MAP.md`, `EEPROM_BACKUP.txt` |
| Load sensing | `LOAD_SENSOR_ANALYSIS.md` |

## Build Environments

| Environment | Purpose | Games | Debug cmds |
|-------------|---------|:-----:|:----------:|
| `nova_voyager` | Production (default) | No | No |
| `nova_voyager_games` | Production + games | Yes | No |
| `release_120` | 120 MHz release | No | No |
| `debug_120` | 120 MHz debug | No | Yes |
| `native` | Unit tests (host PC) | — | — |

## Invariant checks (run in CI)

These encode rules the compiler and the test suite cannot see. Each is
negative-tested — breaking the rule makes the script fail and name the symbol.

- `scripts/check-pins.sh` — toolchain/framework pins are actually applied per-env
- `scripts/check-safety-gate.sh` — every motor-energizing path consults the safety gate
- `scripts/check-settings-mirror.sh` — the native test's copy of the settings
  types matches the real headers (the native build compiles no `src/`, so that
  copy is the only coverage)

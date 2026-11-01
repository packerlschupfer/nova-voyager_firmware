# Contributing

This is firmware for a machine tool with a powered spindle. It participates in
starting and stopping the motor and in the guard and E-Stop paths. That shapes
everything below.

## Before you touch anything

Read [`README.md`](README.md)'s **Safety** section, and understand that there
is exactly one drill press this has been validated on: a Nova Voyager DVR with
HMI board **Teknatool P/N 55400 Rev 03-2024** (GD32F303RCT6). Different
production runs carry different MCUs. An image built for the wrong one will not
run, and the manufacturer pulled their own firmware downloads because people
bricked boards with mismatched files.

Keep a backup of your OEM firmware image and your EEPROM contents before you
flash anything. Restoring the stock firmware is supported
(`./flash_firmware.sh original`) but only if you have those files — this
project ships no manufacturer binaries.

## The routine before you push

Run all of it. CI runs the same things and will tell you anyway, but the point
is to know before you push:

```bash
./scripts/check-builds.sh          # all four environments CI builds
pio test -e native                 # 506 tests
./scripts/check-safety-gate.sh
./scripts/check-settings-mirror.sh
./scripts/check-pins.sh
./scripts/check-version.sh
./scripts/check-settings-keys.sh
```

`check-builds.sh` exists because for a long time only `release` was ever built
by hand. A 1280-byte addition then overflowed the RAM-tight `nova_voyager_games`
and `nova_voyager_demo` builds past the DFU hand-off word, and CI stayed red for
eight commits before anyone looked.

Each gate is negative-tested: break the invariant and the script fails naming
the offending symbol. If a gate fails and you believe the gate is wrong, say so
in the PR — don't route around it.

## Things that will get a change sent back

- **Energising the motor without going through `safety_can_start_motor()`.**
  There is one choke point, enforced by `check-safety-gate.sh`, with a
  two-entry allowlist that carries its reasons in the script. v0.1.0 shipped
  with `ALIGN` bypassing it.
- **A jam-detector threshold change with no measurement behind it.** The
  detector has a flight recorder (`JAMTRACE`) precisely so thresholds can be
  argued from recorded samples. "This felt too sensitive" is a reason to go and
  capture a trace, not to change a number.
- **A new setting that `DUMP` prints but `SET` cannot write.** That makes a
  settings export silently partial, and `check-settings-keys.sh` fails on it.
  If a key genuinely must be read-only, put it on that script's allowlist with
  a reason.
- **A new `settings_t` field with no entry in `test/test_settings`'s mirror.**
  `pio test -e native` compiles no `src/`, so that mirror *is* the coverage;
  `check-settings-mirror.sh` enforces it. 39 settings once went unpersisted
  behind a fully green suite.
- **Changing what a stored field means without bumping its layout version.**
  A checksum covers the bytes and says nothing about how they are interpreted.
  See the comments on `EE_CUSTOM_VERSION_NUM` and `SETTINGS_MAGIC`.
- **`__DATE__` / `__TIME__` or anything else that makes two builds of the same
  commit differ.** Reproducibility is checked; the version string comes from
  `git describe` for this reason.

## Commit messages

Long and explanatory, in the imperative. They are the project's real
documentation: *why* a threshold is what it is, what the machine actually did,
which measurement settled it. If you fixed something subtle, the next person
reading `git log` should not have to re-derive your reasoning. Dates and
observed values are welcome and used throughout.

## History and branches

`main` is **append-only**. Force-pushing it is blocked at the server, including
for the owner. Work on a branch and open a PR. Before 2026-10-31 this project
was developed with squash-and-amend over `main`; it had one contributor and no
forks, and that is no longer true.

## Hardware you need to do anything on-target

- ST-Link v2 and the willingness to open the control panel. The `X4` header is
  `CLK G SW V` — see [`docs/HARDWARE_SETUP.md`](docs/HARDWARE_SETUP.md).
- Optionally the companion
  [bootloader](https://github.com/Packerlschupfer/nova-voyager_bootloader),
  cloned as a sibling directory, for a first-time install.
- `flash_firmware.sh` defaults to driving the probe from a Raspberry Pi over
  ssh, because that is the author's bench. Set `PI_HOST=""` for a locally
  attached ST-Link, or `PI_HOST=user@host` for your own.

Most work needs none of this: the native test suite runs anywhere, and all four
firmware environments cross-compile without hardware.

## Reporting a problem

Paste the output of `STATUS`. It begins with the exact build identity — the
`git describe` string and which of the three environments it is — and without
that line a report cannot be acted on. If a jam detector stopped a cut, add
`JAMTRACE`; reading it is non-destructive and it holds the load and speed
samples leading up to the trip.

# Changelog

Notable changes per release. The full reasoning for anything here is in the
commit it came from — `git log` is this project's real documentation, and the
messages are written to be read.

Versions are [semantic](https://semver.org/) and stay on `0.x`: nothing here is
a stable interface yet.

## v0.1.0 — 2026-10-31

The first release anyone other than the author could actually install. The
earlier `v0.1.0` tag and its assets were republished roughly fifty times
between 2026-08-29 and 2026-09-05 and were never downloaded once; this is the
final, settled `v0.1.0`, and the tag does not move again.

### If you are upgrading from an earlier v0.1.0 build

**Your stored motor settings will be reset, deliberately.** The EEPROM layout
counter went back to 1, so a block written by an older build is rejected — the
console says `EEPROM block is layout v2, firmware expects v1`, your block is
left on the chip untouched, and defaults apply until you save.

This matters because the register remap below changed what three stored fields
*mean* without changing the struct that holds them: `torque_ramp` was 50–2000
with a factory 75 and is now the MCB's `SR` at 1000–10000; `pulse_max` went
185 → 50; `ir_offset` 400 → 82. A checksum covers the stored bytes and says
nothing about how they are read, so those values would have passed validation
while being wrong. Check **Menu > Motor** before cutting.

### Motor controller

- **The MCB parameter table was recovered and the registers we write were
  corrected.** `TR` is the MCB's *thermal threshold*, not the torque ramp,
  which is `SR` — this firmware had been writing the wrong register for years.
  Factory constants re-rated against the drive's own reported min/max.
- IR compensation is unimplemented on this drive; there is no hidden write
  path, and the setting is documented as inert rather than removed.
- Motor profile and PID gain changes no longer revert on the next poll.

### Jam and load detection

The detector was rewritten after it emergency-stopped the machine twice
mid-cut, drilling aluminium with a 17 mm bi-metal hole saw.

- **Spike Thresh is now the authority.** It used to be floored by
  `min(baseline + delta, setting)`, which made the operator's setting incapable
  of relaxing anything — with a free-run baseline of ~11% the tier tripped at
  ~36% whatever the menu said, and no setting existed that let a hole saw
  finish a cut.
- **Grab detection has two independent paths**, each needing two consecutive
  qualified samples: a raw load *slam* that lands at or above 80%, and a
  *droop* where the spindle is out of regulation under real load. They no
  longer share a confirm counter, so a breakthrough cannot finish confirming a
  slam that already cancelled.
- **The droop path waits until the spindle has actually reached its setpoint**,
  for eight consecutive in-band polls. Both halves of that were real
  false-trip causes: comparing a commanded speed against a CV reading taken
  before the dial moved, and arming on a narrower band than the one that trips.
- **The sustained tier can no longer sit below the load the machine idles at.**
  Free-run load scales with speed — 15–16% at 750 rpm, 24–25% at 1250 — so the
  learned baseline is now a floor on that threshold. It can only raise it.
- **A flight recorder** (`JAMTRACE`) holds the load and speed samples leading
  up to a trip, survives a reset, and is non-destructive to read. Its
  timestamps are measured per sample rather than assumed from the nominal poll
  rate, because the real poll is not 50 ms.

### Motor serial protocol

- **Replies were being lost to their own checksum byte.** `wait_response_for()`
  returned on ETX without consuming the trailing checksum, which then arrived
  as the first byte of the *next* reply. For `SV=500` that checksum is `0x02` —
  which is `STX` — so the CV poll failed on every cycle while a one-shot query
  of the same register worked. 410 and 140 collide the same way.
- Lost replies are now counted. `STATS` previously reported `Timeouts: 0` while
  a query failed every single cycle.
- Checksums are verified and counted, deliberately **not** enforced yet.

### Speeds and materials

- **Hole saws have their own surface-speed table** instead of a twist drill's
  speed multiplied by 0.4, with the metal rows taken from Bosch's own
  published figures.
- The speed calculator is reachable from `SET`/`GET`/`DUMP`.

### Build, release and tooling

- **Builds identify themselves.** The version is `git describe` injected at
  build time, with the environment name alongside it, reported on the console
  banner, in `HELP`, and at the top of `STATUS`. It was previously a
  hand-maintained constant reading `v0.1.0-RTOS`, unchanged for 35 commits.
- **`release.yml` would have published nothing.** It ran the toolchain-pin
  check before installing any packages; the fix CI received on 2026-10-11 was
  never ported across. It now also runs the test suite, fetches full history so
  `git describe` works, and names assets with the version.
- **The documented local-probe mode exists.** `flash_firmware.sh` hardcoded
  ssh to one particular Raspberry Pi, so the project's own tooling could not
  install this firmware on anyone else's bench. `PI_HOST=""` now runs
  everything locally.
- `scripts/check-builds.sh` builds every environment CI builds; `check-version.sh`
  keeps the version block honest. Both are in the pre-push routine in
  [`CONTRIBUTING.md`](CONTRIBUTING.md).
- Stock firmware sets the MCU's flash read protection at runtime, which is why
  an interrupted update can leave a panel that looks dead. Documented, with the
  recovery path.

### Documentation

- `docs/HARDWARE_MCB.md`: a hardware reference for the motor control board,
  with photographs and a continuity-checked `SIP1` pinout.
- `CONTRIBUTING.md`, this changelog, a named copyright holder, and the
  third-party licences present in a built binary.

---

## v0.1.0 — 2026-08-29 (superseded)

First public release, tagged at `cc2e2bd`. Republished repeatedly through
2026-09-05; assets never downloaded. Retained here only so the release history
is not silently rewritten.

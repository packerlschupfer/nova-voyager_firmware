#!/usr/bin/env bash
#
# check-settings-mirror.sh — keep test/test_settings' local copy of the
# settings types in step with the real headers.
#
# WHY THIS EXISTS
# ---------------
# `pio test -e native` builds NO src/ (build_src_filter = -<*>), so a test that
# needs settings_t cannot include the firmware's definition — test_settings
# carries its own mirrored copy. That mirror is the only coverage there is, and
# when it drifts the suite keeps passing while testing a layout the firmware
# does not ship. It had drifted three ways before this script existed.
#
# WHAT THE FIRST VERSION GOT WRONG (all found by review, on the very commit
# that added it — a weak gate is worse than none, because CI green is then
# read as evidence):
#   1. It checked a HAND-LISTED five structs while the mirror defines ten. The
#      list is now derived FROM THE MIRROR, so a newly mirrored struct is
#      covered the moment it appears.
#   2. It compared field NAMES only, so widening uint16_t -> uint32_t in the
#      header read as "fields match" while every later offset diverged. Types
#      and array extents are compared now.
#   3. When a struct failed to parse it printed "not mirrored — fine" and
#      PASSED, silently disabling itself. Anything unparseable is now a
#      failure.
#   4. It only opened include/settings.h, so it missed EE_CUSTOM_MAGIC_VALUE
#      moving in eeprom_layout.h — and then, still missing config.h, it passed
#      while the mirror's SPEED_MIN_RPM / MAX / DEFAULT / TAP_DEFAULT were
#      100/6000/1500/300 against the real 50/5500/500/200, so the clamp tests
#      exercised bounds the firmware never uses. All three headers are read.
#   5. `if not checked` counted macros AND structs together, so if every struct
#      stopped parsing the macro comparisons kept the total non-zero and the
#      whole layout gate vanished behind an OK. Counted separately now.
#   6. FIELD_RE silently skipped anything it could not parse — on BOTH sides,
#      so a multi-declarator line or a bitfield added to the header read as
#      "fields match". An unparseable line in a struct body is now a failure.
#
# Run from the repo root. Exits non-zero on drift.

set -uo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
import re, sys

HEADERS = ('include/settings.h', 'include/eeprom_layout.h', 'include/config.h')
MIRROR  = 'test/test_settings/test_main.c'

header_text = '\n'.join(open(h, encoding='utf-8').read() for h in HEADERS)
mirror_text = open(MIRROR, encoding='utf-8').read()

def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)

H, M = strip_comments(header_text), strip_comments(mirror_text)

STRUCT_RE = re.compile(r'typedef\s+struct\s*(?:__attribute__\(\(packed\)\)\s*)?\{'
                       r'((?:[^{}]|\{[^{}]*\})*)\}\s*(\w+)\s*;', re.S)
FIELD_RE  = re.compile(r'^\s*([A-Za-z_][\w ]*?)\s+(\w+)\s*(\[[^\]]*\])?\s*;\s*$')

unparsed = []

def structs(text, where):
    """Parse every typedef'd struct. A line inside a body that FIELD_RE cannot
    consume is recorded rather than skipped — silently dropping it on both
    sides makes a real layout change read as 'fields match'."""
    out = {}
    for body, name in STRUCT_RE.findall(text):
        fields = []
        for line in body.split('\n'):
            if not line.strip():
                continue
            m = FIELD_RE.match(line)
            if m:
                t, n, a = m.groups()
                fields.append((' '.join(t.split()), n, a or ''))
            else:
                unparsed.append('%s: %s: %s' % (where, name, line.strip()))
        out[name] = fields
    return out

def defines(text):
    """Collect #define values. Records any macro defined more than once so the
    caller can refuse to compare it: this scan has no #if awareness, so a macro
    with variant branches (config.h already has SYSCLK_FREQ, APB1_FREQ and
    APB2_FREQ like this) would otherwise be compared against whichever branch
    happens to sit last in the file, which may not be the one that was built."""
    out, dupes = {}, set()
    for n, v in re.findall(r'^#define\s+(\w+)\s+([^\n/]+)', text, re.M):
        if n in out and out[n] != v.strip():
            dupes.add(n)
        out[n] = v.strip()
    return out, dupes

hs, ms = structs(H, 'header'), structs(M, 'mirror')

# REVIEW FIX: the hard-fail on unparseable members was applied to EVERY struct
# in all three headers, including ones the mirror has nothing to do with — so
# adding a pointer, a bitfield or a two-declarator line anywhere in config.h
# failed this gate for no reason. Only members of structs the mirror actually
# mirrors can affect the comparison, so only those are policed.
mirrored = set(ms)
unparsed[:] = [u for u in unparsed
               if u.split(': ')[1] in mirrored]
hd, h_dupes = defines(H)
md, m_dupes = defines(M)
ambiguous = h_dupes | m_dupes

problems = []
n_macros = n_structs = 0

# Macros: only those the mirror redefines AND the headers define.
# SETTINGS_MAGIC is excluded — it lives in config.h and the mirror uses its own
# placeholder, which affects no layout.
for macro in sorted(set(md) & set(hd) - {'SETTINGS_MAGIC'}):
    if macro in ambiguous:
        problems.append('%s: defined more than once with different values '
                        '(preprocessor variants) — this check cannot tell which '
                        'branch is built, so it cannot be compared' % macro)
        continue
    n_macros += 1
    if md[macro] != hd[macro]:
        problems.append('%s: header=%s mirror=%s' % (macro, hd[macro], md[macro]))
    else:
        print('  macro  %-24s %s' % (macro, hd[macro]))

# Structs: every struct the MIRROR defines must exist in a header and match.
for name in sorted(ms):
    n_structs += 1
    if name not in hs:
        problems.append('%s: mirrored but not found in %s — '
                        'renamed, moved, or unparseable' % (name, ' / '.join(HEADERS)))
        continue
    a, b = hs[name], ms[name]
    if a != b:
        only_h = [f for f in a if f not in b]
        only_m = [f for f in b if f not in a]
        problems.append('%s:\n      header-only: %s\n      mirror-only: %s'
                        % (name, only_h or '(none)', only_m or '(none)'))
    else:
        print('  struct %-24s %d fields match (name + type + extent)' % (name, len(a)))

print()

# REVIEW FIX: print `problems` BEFORE the count gates. Ambiguous macros
# `continue` without incrementing n_macros, so if every shared macro were
# duplicate-defined the "defined more than once" entries would be silently
# discarded by the n_macros == 0 exit below, which then blames a rename that
# never happened.
def report_and_exit():
    print('FAIL: test/test_settings mirror has drifted:')
    for p_ in problems:
        print('  ' + p_)
    print()
    print('The native suite compiles no src/, so this mirror IS the coverage.')
    sys.exit(1)

if unparsed:
    problems.append('unparseable struct member(s) — the comparison would have '
                    'silently ignored them:')
    for u in unparsed:
        problems.append('    ' + u)

if problems:
    report_and_exit()

if n_structs == 0:
    print('FAIL: no structs were compared. STRUCT_RE matched nothing in the')
    print('mirror — a rename, a tagged typedef, or deeper brace nesting — so')
    print('the layout gate silently disappeared. Counting macros too would')
    print('have hidden this behind an OK.')
    sys.exit(1)
if n_macros == 0:
    print('FAIL: no macros were compared; the mirror or the headers moved.')
    sys.exit(1)

print('OK: %d macro(s) and %d struct(s) match the headers.' % (n_macros, n_structs))
PY

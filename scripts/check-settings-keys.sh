#!/usr/bin/env bash
set -euo pipefail
#
# check-settings-keys.sh — DUMP, GET, SET and EXPORT must agree on the keys.
#
# WHY THIS EXISTS
# ---------------
# There were three independent hand-maintained key lists in commands.c: the
# one DUMP prints, the one settings_read_by_key() (GET) answers, and the
# strcmp chain in cmd_set(). On 2026-11-02 DUMP printed 34 keys and SET could
# write 14 of them.
#
# That is not a cosmetic inconsistency. It means a settings export can only
# ever restore a third of a machine's configuration, and it does so silently —
# every line it replays succeeds, so it looks like it worked. It cost a real
# set of settings on 2026-11-01, when a full-chip erase wiped the flash-held
# fields and the only record of them was a chat log.
#
# It also breaks SET's own output: cmd_set() echoes the STORED value by reading
# it back through settings_read_by_key(), so a key that SET writes but GET
# cannot read reports the value the operator typed rather than the value that
# was stored — hiding exactly the clamping the read-back exists to reveal.
#
# So: every key DUMP prints must be readable by GET, and writable by SET unless
# it is on the read-only allowlist below with a reason.
#
cd "$(dirname "$0")/.." || { echo "cannot reach the repo root" >&2; exit 1; }

# Overridable so the gate can be negative-tested against a doctored copy
# without touching the real file.
SRC="${SETTINGS_KEYS_SRC:-src/commands.c}"

# Keys that are deliberately readable but not writable. Each needs a reason,
# the same convention check-safety-gate.sh uses for its allowlist.
declare -A READONLY=(
  [power.dc_bus]="live DC bus measurement, not a setting"
)

fail=0

# --- DUMP: literal "prefix.key=" strings, plus the favourites it builds up
#     one character at a time (uart_puts("speed.fav"); uart_putc('0'+i)). ---
dump_keys=$(grep -oE 'uart_puts\("[a-z]+\.[a-z0-9_]+=' "$SRC" \
            | sed 's/uart_puts("//; s/=$//' | sort -u)
if grep -q 'uart_puts("speed.fav")' "$SRC"; then
    for i in 0 1 2 3 4 5 6 7; do dump_keys+=$'\n'"speed.fav$i"; done
fi
dump_keys=$(printf '%s\n' "$dump_keys" | sed '/^$/d' | sort -u)

# --- GET: the strcmp chain inside settings_read_by_key(), plus its fav range ---
get_body=$(sed -n '/^static bool settings_read_by_key(const char\* key, uint16_t\* out) {/,/^}/p' "$SRC")
get_keys=$(printf '%s\n' "$get_body" | grep -oE 'strcmp\(key, "[a-z]+\.[a-z0-9_]+"\)' \
           | sed 's/.*"\(.*\)".*/\1/' | sort -u)
if printf '%s\n' "$get_body" | grep -q 'strncmp(key, "speed.fav", 9)'; then
    for i in 0 1 2 3 4 5 6 7; do get_keys+=$'\n'"speed.fav$i"; done
fi
get_keys=$(printf '%s\n' "$get_keys" | sed '/^$/d' | sort -u)

# --- SET: the strcmp chain inside cmd_set(), plus its fav range ---
set_body=$(sed -n '/^void cmd_set(void) {/,/^}/p' "$SRC")
set_keys=$(printf '%s\n' "$set_body" | grep -oE 'strcmp\(key, "[a-z]+\.[a-z0-9_]+"\)' \
           | sed 's/.*"\(.*\)".*/\1/' | sort -u)
if printf '%s\n' "$set_body" | grep -q 'strncmp(key, "speed.fav", 9)'; then
    for i in 0 1 2 3 4 5 6 7; do set_keys+=$'\n'"speed.fav$i"; done
fi
set_keys=$(printf '%s\n' "$set_keys" | sed '/^$/d' | sort -u)

# cmd_set() NAMES the read-only keys in order to reject them with a useful
# message rather than "unknown key" — which would send someone hunting for a
# typo. Appearing in the chain therefore does not mean writable, so the
# allowlist wins. (This gate's first run got that wrong and reported
# power.dc_bus as "writable, but EXPORT would skip it".)
for ro in "${!READONLY[@]}"; do
    set_keys=$(printf '%s\n' "$set_keys" | grep -vx "$ro" || true)
done

# --- EXPORT: the EXPORT_KEYS[] table ---
export_keys=$(sed -n '/^static const char\* const EXPORT_KEYS\[\] = {/,/^};/p' "$SRC" \
              | grep -oE '"[a-z]+\.[a-z0-9_]+"' | tr -d '"' | sort -u)

printf 'DUMP %s keys, GET %s, SET %s, EXPORT %s\n' \
    "$(printf '%s\n' "$dump_keys"   | wc -l)" \
    "$(printf '%s\n' "$get_keys"    | wc -l)" \
    "$(printf '%s\n' "$set_keys"    | wc -l)" \
    "$(printf '%s\n' "$export_keys" | wc -l)"

# 1. Every DUMP key must be readable by GET.
while read -r k; do
    [ -z "$k" ] && continue
    if ! printf '%s\n' "$get_keys" | grep -qx "$k"; then
        echo "  MISSING from GET: $k   (DUMP prints it; GET cannot read it)" >&2
        fail=1
    fi
done <<< "$dump_keys"

# 2. Every DUMP key must be writable by SET, or allowlisted read-only.
while read -r k; do
    [ -z "$k" ] && continue
    if printf '%s\n' "$set_keys" | grep -qx "$k"; then continue; fi
    if [ -n "${READONLY[$k]:-}" ]; then
        echo "  read-only  $k — ${READONLY[$k]}"
        continue
    fi
    echo "  MISSING from SET: $k   (DUMP prints it; nothing can restore it)" >&2
    fail=1
done <<< "$dump_keys"

# 3. Every SET key must be readable, or cmd_set's read-back echo lies.
while read -r k; do
    [ -z "$k" ] && continue
    if ! printf '%s\n' "$get_keys" | grep -qx "$k"; then
        echo "  MISSING from GET: $k   (SET writes it, so the echo cannot" >&2
        echo "                         report clamping and prints the typed value)" >&2
        fail=1
    fi
done <<< "$set_keys"

# 4. EXPORT must cover every writable key, or a restore is quietly partial.
while read -r k; do
    [ -z "$k" ] && continue
    if ! printf '%s\n' "$export_keys" | grep -qx "$k"; then
        echo "  MISSING from EXPORT: $k   (writable, but a restore would skip it)" >&2
        fail=1
    fi
done <<< "$set_keys"

# 5. EXPORT must not emit anything SET cannot take back.
while read -r k; do
    [ -z "$k" ] && continue
    if ! printf '%s\n' "$set_keys" | grep -qx "$k"; then
        echo "  EXPORT emits a key SET cannot write: $k" >&2
        fail=1
    fi
done <<< "$export_keys"

# 6. speed.max must come first in EXPORT_KEYS — settings_set_speed() and
#    settings_set_favorite_speed() clamp to it, so a restore that sets the
#    cap afterwards brings the speeds back clamped.
first=$(sed -n '/^static const char\* const EXPORT_KEYS\[\] = {/,/^};/p' "$SRC" \
        | grep -oE '"[a-z]+\.[a-z0-9_]+"' | head -1 | tr -d '"')
if [ "$first" != "speed.max" ]; then
    echo "  EXPORT_KEYS must start with speed.max, found: ${first:-<none>}" >&2
    echo "  (speed.default and the favourites clamp to it on restore)" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo >&2
    echo "Settings key lists disagree. A settings export that cannot be" >&2
    echo "replayed in full is worse than none: every line succeeds, so it" >&2
    echo "looks like the machine was restored." >&2
    exit 1
fi

echo "OK: DUMP ⊆ GET, DUMP ⊆ SET ∪ read-only, SET ⊆ GET, SET = EXPORT, order sound."

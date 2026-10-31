#!/usr/bin/env bash
set -euo pipefail
#
# check-version.sh — the release tag and the fallback version must agree.
#
# WHY THIS EXISTS
# ---------------
# The firmware reports `git describe`, so a tagged build is self-identifying
# and this check is belt-and-braces for it. What it really guards is the
# FALLBACK: FW_VERSION_FALLBACK in include/config.h is what a source-tarball
# build calls itself, and nothing stops it drifting from the tag. It already
# drifted once, spectacularly — FW_VERSION_STRING said "v0.1.0-RTOS" for 35
# commits and roughly 50 republished release builds, so every bug report
# quoted the same string no matter what was flashed.
#
# Usage:
#   ./scripts/check-version.sh            # compare against `git describe`
#   ./scripts/check-version.sh v0.1.0     # compare against an explicit tag
#
cd "$(dirname "$0")/.." || { echo "cannot reach the repo root" >&2; exit 1; }

hdr="include/config.h"

major=$(grep -oP '^#define\s+FW_VERSION_MAJOR\s+\K[0-9]+' "$hdr")
minor=$(grep -oP '^#define\s+FW_VERSION_MINOR\s+\K[0-9]+' "$hdr")
patch=$(grep -oP '^#define\s+FW_VERSION_PATCH\s+\K[0-9]+' "$hdr")
fallback=$(grep -oP '^#define\s+FW_VERSION_FALLBACK\s+"\K[^"]+' "$hdr")

from_numbers="v${major}.${minor}.${patch}"

fail=0

# 1. The three numbers and the fallback string describe the same version.
if [ "$from_numbers" != "$fallback" ]; then
    echo "MISMATCH: FW_VERSION_MAJOR/MINOR/PATCH say $from_numbers," >&2
    echo "          FW_VERSION_FALLBACK says $fallback" >&2
    fail=1
fi

# 2. If a tag was named (or HEAD is exactly on one), it must match too.
tag="${1:-}"
if [ -z "$tag" ] && git describe --tags --exact-match HEAD >/dev/null 2>&1; then
    tag=$(git describe --tags --exact-match HEAD)
fi

if [ -n "$tag" ]; then
    if [ "$tag" != "$fallback" ]; then
        echo "MISMATCH: tag $tag does not match FW_VERSION_FALLBACK $fallback" >&2
        echo "          Bump the version block in $hdr, or tag the right version." >&2
        fail=1
    else
        echo "ok    tag $tag matches $hdr"
    fi
else
    echo "note  HEAD is not on a tag; checked $hdr internal consistency only"
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "ok    version block consistent ($fallback)"

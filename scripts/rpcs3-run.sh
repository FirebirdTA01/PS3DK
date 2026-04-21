#!/usr/bin/env bash
# rpcs3-run.sh — boot a .fake.self in RPCS3, capture printf output,
# exit after a timeout.
#
# Usage:
#   scripts/rpcs3-run.sh <path/to/sample.fake.self> [timeout_seconds]
#
# Behaviour:
#   - Clears ~/.cache/rpcs3/RPCS3.log.
#   - Launches `rpcs3 --no-gui <path>`.
#   - After the timeout (default 20s) kills RPCS3.
#   - Extracts all sys_tty_write payload lines — the homebrew's
#     printf output — from the log, one per line.
#   - Exit status: 0 if the log contained any sys_tty_write output,
#     1 otherwise (presumed boot-time crash).
#
# The harness is single-purpose and Linux-only. --no-gui works on
# this machine; --headless is rejected by the Qt music handler.

set -euo pipefail

usage() {
    echo "Usage: $0 <sample.fake.self> [timeout_seconds]" >&2
    exit 2
}

[[ $# -ge 1 ]] || usage

SELF_PATH="$1"
TIMEOUT="${2:-20}"

[[ -f "$SELF_PATH" ]] || { echo "rpcs3-run: file not found: $SELF_PATH" >&2; exit 2; }

LOG="$HOME/.cache/rpcs3/RPCS3.log"
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

echo "rpcs3-run: booting $SELF_PATH (timeout ${TIMEOUT}s)"

timeout --signal=KILL "$TIMEOUT" \
    rpcs3 --no-gui "$SELF_PATH" >/dev/null 2>&1 || true

echo "rpcs3-run: tty output:"
echo "---"
# Extract sys_tty_write payload. Format:
#   ...sys_tty: sys_tty_write(): "payload" << endl
# The build here uses unicode curly quotes; grep with -P handles both.
grep 'sys_tty_write(): ' "$LOG" \
    | sed -E 's/^.*sys_tty_write\(\): (.*) << endl.*$/\1/' \
    | sed -E 's/^[[:space:]]*[\xe2\x80\x9c"]//' \
    | sed -E 's/[\xe2\x80\x9d"][[:space:]]*$//'

echo "---"

if grep -q 'sys_tty_write(): ' "$LOG"; then
    exit 0
else
    echo "rpcs3-run: no tty output captured; possible boot-time crash." >&2
    echo "rpcs3-run: full log: $LOG" >&2
    exit 1
fi

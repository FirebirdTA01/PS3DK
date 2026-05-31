#!/usr/bin/env bash
# Multi-frame capture harness for flicker detection.
# Captures N frames at regular intervals and saves numbered PNGs.
# Usage: $0 <sample.fake.self> [count] [interval_ms] [output_dir]
set -euo pipefail

SELF_PATH="$1"
COUNT="${2:-20}"
INTERVAL_MS="${3:-150}"
OUT_DIR="${4:-build/multi-frame-captures/$(basename "${SELF_PATH%.*}")}"
RPCS3_BIN="${RPCS3_BIN:-rpcs3}"
LOG="$HOME/.cache/rpcs3/RPCS3.log"

[[ -f "$SELF_PATH" ]] || { echo "multicap: file not found: $SELF_PATH" >&2; exit 2; }
mkdir -p "$OUT_DIR" "$(dirname "$LOG")"
: > "$LOG"

# Kill stale rpcs3
if pgrep -x rpcs3 >/dev/null; then
    pkill -x rpcs3 || true
    for _ in 1 2 3 4 5; do pgrep -x rpcs3 >/dev/null || break; sleep 1; done
    if pgrep -x rpcs3 >/dev/null; then pkill -9 -x rpcs3 || true; sleep 2; fi
fi

echo "multicap: booting $SELF_PATH (${COUNT} frames, ${INTERVAL_MS}ms)"
QT_XCB_NO_SEND_EVENT_CHECK=1 "$RPCS3_BIN" "$SELF_PATH" \
    >"$OUT_DIR/stdout.log" 2>"$OUT_DIR/stderr.log" &
RPCS3_PID=$!

# Wait for game window (priority: FPS: or sample basename)
SAMPLE_NAME="$(basename "$SELF_PATH" .fake.self)"
SAMPLE_NAME="${SAMPLE_NAME%.self}"
WID=""
for _ in $(seq 1 30); do
    if ! grep -q "Creating new game window" "$LOG" 2>/dev/null; then
        if ! kill -0 "$RPCS3_PID" 2>/dev/null; then break; fi
        sleep 1; continue
    fi
    best_wid=""; best_prio=0
    for w in $(xdotool search --pid "$RPCS3_PID" 2>/dev/null || true); do
        name="$(xdotool getwindowname "$w" 2>/dev/null || true)"
        case "$name" in "RPCS3 0.0."*|"") continue;; esac
        prio=1
        case "$name" in *"FPS:"*|*"$SAMPLE_NAME"*) prio=2;; esac
        if (( prio > best_prio )); then best_wid="$w"; best_prio=$prio; fi
    done
    if [[ -n "$best_wid" ]]; then WID="$best_wid"; break; fi
    sleep 1
done

if [[ -z "$WID" ]]; then
    echo "multicap: no game window found" >&2
    kill "$RPCS3_PID" 2>/dev/null || true
    exit 1
fi
echo "multicap: window WID=$WID"

# Wait for TTY ready
for _ in $(seq 1 30); do
    if grep -q 'sys_tty_write():.*ready' "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$RPCS3_PID" 2>/dev/null; then break; fi
    sleep 1
done

# Capture N frames
echo "multicap: capturing ${COUNT} frames at ${INTERVAL_MS}ms intervals"
xdotool windowraise "$WID" >/dev/null 2>&1 || true
xdotool windowactivate --sync "$WID" >/dev/null 2>&1 || true
for i in $(seq 0 $((COUNT - 1))); do
    shot="$OUT_DIR/frame_$(printf '%04d' "$i").png"
    if DISPLAY=:0 timeout 5 import -window "$WID" "$shot" 2>/dev/null; then
        echo "multicap: $shot ($(stat -c%s "$shot") bytes)"
    else
        echo "multicap: frame $i capture failed" >&2
    fi
    if ! kill -0 "$RPCS3_PID" 2>/dev/null; then break; fi
    sleep "$(bc <<< "scale=3; ${INTERVAL_MS}/1000")"
done

# Kill rpcs3 if still running
if kill -0 "$RPCS3_PID" 2>/dev/null; then
    kill "$RPCS3_PID" 2>/dev/null || true
    sleep 1
fi

cp "$LOG" "$OUT_DIR/RPCS3.log"
echo "multicap: done, $(ls "$OUT_DIR"/frame_*.png 2>/dev/null | wc -l) frames captured"

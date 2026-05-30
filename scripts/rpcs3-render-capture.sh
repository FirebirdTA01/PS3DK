#!/usr/bin/env bash
# Boot a rendering sample in RPCS3, save a window screenshot, and print TTY.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <sample.fake.self> [timeout_seconds] [output_dir] [x y clear_r clear_g clear_b [tolerance]]" >&2
    exit 2
fi

SELF_PATH="$1"
TIMEOUT="${2:-15}"
OUT_DIR="${3:-build/rpcs3-render-captures/$(basename "${SELF_PATH%.*}")}"
RPCS3_BIN="${RPCS3_BIN:-rpcs3}"
LOG="$HOME/.cache/rpcs3/RPCS3.log"
CHECK_X="${4:-}"
CHECK_Y="${5:-}"
CLEAR_R="${6:-}"
CLEAR_G="${7:-}"
CLEAR_B="${8:-}"
TOLERANCE="${9:-4}"

[[ -f "$SELF_PATH" ]] || { echo "rpcs3-capture: file not found: $SELF_PATH" >&2; exit 2; }

mkdir -p "$OUT_DIR" "$(dirname "$LOG")"
: > "$LOG"

if pgrep -x rpcs3 >/dev/null; then
    pkill -x rpcs3 || true
    for _ in 1 2 3 4 5; do
        pgrep -x rpcs3 >/dev/null || break
        sleep 1
    done
    if pgrep -x rpcs3 >/dev/null; then
        pkill -9 -x rpcs3 || true
        sleep 1
    fi
fi

echo "rpcs3-capture: booting $SELF_PATH (timeout ${TIMEOUT}s)"
QT_XCB_NO_SEND_EVENT_CHECK=1 "$RPCS3_BIN" "$SELF_PATH" \
    >"$OUT_DIR/stdout.log" 2>"$OUT_DIR/stderr.log" &
RPCS3_PID=$!

WID=""
for _ in $(seq 1 "$TIMEOUT"); do
    for w in $(xdotool search --pid "$RPCS3_PID" 2>/dev/null || true); do
        name="$(xdotool getwindowname "$w" 2>/dev/null || true)"
        case "$name" in
            "RPCS3 0.0."*|"")
                ;;
            *)
                WID="$w"
                break
                ;;
        esac
    done
    if [[ -n "$WID" ]] && grep -q "VK_PRESENT_ENTRY" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$RPCS3_PID" 2>/dev/null; then
        break
    fi
    sleep 1
done

SHOT="$OUT_DIR/screenshot.png"
for _ in 1 2 3 4 5; do
    if [[ -n "$WID" ]] && xdotool getwindowname "$WID" >/dev/null 2>&1; then
        xdotool windowraise "$WID" >/dev/null 2>&1 || true
        xdotool windowactivate --sync "$WID" >/dev/null 2>&1 || true
        if DISPLAY="${DISPLAY:-:0}" timeout 5 import -window "$WID" "$SHOT"; then
            echo "rpcs3-capture: screenshot saved: $SHOT ($(stat -c%s "$SHOT") bytes)"
            break
        fi
    fi
    WID=""
    for w in $(xdotool search --pid "$RPCS3_PID" 2>/dev/null || true); do
        name="$(xdotool getwindowname "$w" 2>/dev/null || true)"
        case "$name" in
            "RPCS3 0.0."*|"")
                ;;
            *)
                WID="$w"
                break
                ;;
        esac
    done
    sleep 1
done
if [[ ! -s "$SHOT" ]]; then
    echo "rpcs3-capture: no capturable RPCS3 render window found" >&2
fi

deadline=$((SECONDS + TIMEOUT))
while kill -0 "$RPCS3_PID" 2>/dev/null && (( SECONDS < deadline )); do
    sleep 1
done
if kill -0 "$RPCS3_PID" 2>/dev/null; then
    kill -KILL "$RPCS3_PID" 2>/dev/null || true
fi

echo "rpcs3-capture: tty output:"
echo "---"
grep 'sys_tty_write(): ' "$LOG" 2>/dev/null \
    | sed -E 's/^.*sys_tty_write\(\): (.*) << endl.*$/\1/' \
    | sed -E 's/^[[:space:]]*[\xe2\x80\x9c"]//' \
    | sed -E 's/[\xe2\x80\x9d"][[:space:]]*$//' || true
echo "---"

cp "$LOG" "$OUT_DIR/RPCS3.log"

if grep -q "fatal error\|Dead FIFO commands queue" "$LOG" 2>/dev/null; then
    echo "rpcs3-capture: fatal RPCS3 log entry found" >&2
    exit 1
fi
if [[ ! -s "$SHOT" ]]; then
    echo "rpcs3-capture: screenshot missing or empty" >&2
    exit 1
fi
if [[ -n "$CHECK_X" && -n "$CHECK_Y" && -n "$CLEAR_R" &&
      -n "$CLEAR_G" && -n "$CLEAR_B" ]]; then
    pixel="$(convert "$SHOT" -crop "1x1+${CHECK_X}+${CHECK_Y}" \
        -format "%[fx:round(255*r)],%[fx:round(255*g)],%[fx:round(255*b)]" info:)"
    IFS=, read -r r g b <<<"$pixel"
    dr=$(( r > CLEAR_R ? r - CLEAR_R : CLEAR_R - r ))
    dg=$(( g > CLEAR_G ? g - CLEAR_G : CLEAR_G - g ))
    db=$(( b > CLEAR_B ? b - CLEAR_B : CLEAR_B - b ))
    echo "rpcs3-capture: pixel ${CHECK_X},${CHECK_Y} = ${pixel}; clear=${CLEAR_R},${CLEAR_G},${CLEAR_B}"
    if (( dr <= TOLERANCE && dg <= TOLERANCE && db <= TOLERANCE )); then
        echo "rpcs3-capture: sampled pixel still matches clear color" >&2
        exit 1
    fi
fi

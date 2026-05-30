#!/usr/bin/env bash
# Boot a rendering sample in RPCS3, save a window screenshot, and print TTY.
# Optional deterministic pixel-inspection mode with per-checkpoint PASS/FAIL.
#
# Usage:
#   $0 <sample.fake.self> [timeout] [output_dir]
#   $0 <sample.fake.self> [timeout] [output_dir] checkpoints:<file>
#
# Checkpoint file format (one per line):
#   x y expected_R expected_G expected_B tolerance
#
# Fixes vs original harness (deepseek, 2026-05-30):
#   - WINDOW-FIND: uses xdotool --pid and accepts any non-empty window
#     name because direct .self samples can render into the main GUI window.
#   - FATAL-GREP: matches actual RPCS3 fatal-marker log lines, not
#     substring false-positives like "Show fatal error hints: false".
#   - PIXEL-INSPECTION: multi-checkpoint mode via external checkpoint file;
#     each checkpoint reports PASS/FAIL with actual vs expected diagnostics.

set -euo pipefail

# ── argument parsing ───────────────────────────────────────────────
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <sample.fake.self> [timeout_seconds] [output_dir] [checkpoints:<file>]" >&2
    exit 2
fi

SELF_PATH="$1"
TIMEOUT="${2:-15}"
OUT_DIR="${3:-build/rpcs3-render-captures/$(basename "${SELF_PATH%.*}")}"
RPCS3_BIN="${RPCS3_BIN:-rpcs3}"
LOG="$HOME/.cache/rpcs3/RPCS3.log"
CHECKP_FILE=""
if [[ "${4:-}" == checkpoints:* ]]; then
    CHECKP_FILE="${4#checkpoints:}"
    [[ -f "$CHECKP_FILE" ]] || { echo "rpcs3-capture: checkpoint file not found: $CHECKP_FILE" >&2; exit 2; }
fi

[[ -f "$SELF_PATH" ]] || { echo "rpcs3-capture: file not found: $SELF_PATH" >&2; exit 2; }

mkdir -p "$OUT_DIR" "$(dirname "$LOG")"

# ── pre-flight: kill stale rpcs3 ───────────────────────────────────
if pgrep -x rpcs3 >/dev/null; then
    echo "rpcs3-capture: killing stale rpcs3"
    pkill -x rpcs3 || true
    for _ in 1 2 3 4 5; do
        pgrep -x rpcs3 >/dev/null || break
        sleep 1
    done
    if pgrep -x rpcs3 >/dev/null; then
        echo "rpcs3-capture: force-killing rpcs3"
        pkill -9 -x rpcs3 || true
        sleep 2
    fi
fi
if pgrep -x rpcs3 >/dev/null; then
    echo "rpcs3-capture: ERROR cannot clear stale rpcs3" >&2
    exit 1
fi

: > "$LOG"

# ── launch rpcs3 ───────────────────────────────────────────────────
echo "rpcs3-capture: booting $SELF_PATH (timeout ${TIMEOUT}s)"
QT_XCB_NO_SEND_EVENT_CHECK=1 "$RPCS3_BIN" "$SELF_PATH" \
    >"$OUT_DIR/stdout.log" 2>"$OUT_DIR/stderr.log" &
RPCS3_PID=$!

# ── window-find + screenshot capture ──────────────────────────────
# Wait for an RPCS3-owned window, then capture immediately while the
# window is still alive.
WID=""
SHOT="$OUT_DIR/screenshot.png"
echo "rpcs3-capture: waiting for GS window (pid=$RPCS3_PID, max ${TIMEOUT}s)"
for _ in $(seq 1 "$TIMEOUT"); do
    for w in $(xdotool search --pid "$RPCS3_PID" 2>/dev/null || true); do
        name="$(xdotool getwindowname "$w" 2>/dev/null || true)"
        # Skip blank-named transient windows only.  Short-lived samples
        # may render into the main RPCS3 window before a separate GS
        # window appears, so we accept any non-empty title.
        if [[ -n "$name" ]]; then
            WID="$w"
            break
        fi
    done
    if [[ -n "$WID" ]]; then
        echo "rpcs3-capture: GS window found: WID=$WID name=\"$(xdotool getwindowname "$WID" 2>/dev/null)\""
        # Wait 3s for first frame to render before capture.
        sleep 3
        xdotool windowraise "$WID" >/dev/null 2>&1 || true
        xdotool windowactivate --sync "$WID" >/dev/null 2>&1 || true
        sleep 0.5
        if DISPLAY="${DISPLAY:-:0}" timeout 5 import -window "$WID" "$SHOT" 2>/dev/null; then
            echo "rpcs3-capture: screenshot saved: $SHOT ($(stat -c%s "$SHOT") bytes)"
        else
            echo "rpcs3-capture: screenshot capture failed" >&2
        fi
        break
    fi
    if ! kill -0 "$RPCS3_PID" 2>/dev/null; then
        echo "rpcs3-capture: rpcs3 exited while waiting for window" >&2
        break
    fi
    sleep 1
done

if [[ ! -s "$SHOT" ]]; then
    echo "rpcs3-capture: no capturable RPCS3 render window found" >&2
fi

# ── wait for sample to finish ─────────────────────────────────────
deadline=$((SECONDS + TIMEOUT))
while kill -0 "$RPCS3_PID" 2>/dev/null && (( SECONDS < deadline )); do
    sleep 1
done
if kill -0 "$RPCS3_PID" 2>/dev/null; then
    echo "rpcs3-capture: timeout, killing rpcs3"
    kill -KILL "$RPCS3_PID" 2>/dev/null || true
    sleep 1
fi

# ── tty output ─────────────────────────────────────────────────────
echo "rpcs3-capture: tty output:"
echo "---"
grep 'sys_tty_write(): ' "$LOG" 2>/dev/null \
    | sed -E 's/^.*sys_tty_write\(\): (.*) << endl.*$/\1/' \
    | sed -E 's/^[[:space:]]*[\xe2\x80\x9c"]//' \
    | sed -E 's/[\xe2\x80\x9d"][[:space:]]*$//' || true
echo "---"

# ── fatal-grep (precise) ──────────────────────────────────────────
cp "$LOG" "$OUT_DIR/RPCS3.log"

FATAL=0
# RPCS3 fatal signatures only — NOT all ·E error lines (non-fatal GUI shutdown
# errors appear after clean exits). ·F lines (fatal), plus specific known
# fatal substrings.
if grep -qP '(^·F |Dead FIFO commands queue|VM: Access violation|Thread terminated due to fatal error|SIGSEGV|Segfault)' "$LOG" 2>/dev/null; then
    echo "rpcs3-capture: fatal signature found in RPCS3 log:" >&2
    grep -P '(^·F |Dead FIFO commands queue|VM: Access violation|Thread terminated due to fatal error|SIGSEGV|Segfault)' "$LOG" | head -20 >&2
    FATAL=1
fi

# ── screenshot gate ────────────────────────────────────────────────
if [[ ! -s "$SHOT" ]]; then
    echo "rpcs3-capture: screenshot missing or empty" >&2
    exit 1
fi

# ── pixel-inspection mode ──────────────────────────────────────────
if [[ -n "$CHECKP_FILE" ]]; then
    echo "rpcs3-capture: pixel-inspection checkpoints from $CHECKP_FILE"
    ALL_PASS=1
    lineno=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        lineno=$((lineno + 1))
        # Skip blank lines and comments.
        [[ -z "$line" || "$line" == \#* ]] && continue
        read -r cx cy er eg eb tol <<<"$line"
        if [[ -z "$cx" || -z "$cy" || -z "$er" || -z "$eg" || -z "$eb" || -z "$tol" ]]; then
            echo "rpcs3-capture: checkpoint line $lineno: malformed (expected: x y R G B tolerance)" >&2
            ALL_PASS=0
            continue
        fi
        pixel="$(convert "$SHOT" -crop "1x1+${cx}+${cy}" \
            -format "%[fx:round(255*r)],%[fx:round(255*g)],%[fx:round(255*b)]" info: 2>/dev/null)"
        if [[ -z "$pixel" ]]; then
            echo "rpcs3-capture: checkpoint line $lineno: ($cx,$cy) — ImageMagick convert failed" >&2
            ALL_PASS=0
            continue
        fi
        IFS=, read -r r g b <<<"$pixel"
        dr=$(( r > er ? r - er : er - r ))
        dg=$(( g > eg ? g - eg : eg - g ))
        db=$(( b > eb ? b - eb : eb - b ))
        if (( dr <= tol && dg <= tol && db <= tol )); then
            echo "  PASS line $lineno: ($cx,$cy) pixel=($r,$g,$b) expected=($er,$eg,$eb) tol=$tol"
        else
            echo "  FAIL line $lineno: ($cx,$cy) pixel=($r,$g,$b) expected=($er,$eg,$eb) tol=$tol (diffs: $dr,$dg,$db)" >&2
            ALL_PASS=0
        fi
    done < "$CHECKP_FILE"
    if (( ALL_PASS == 0 )); then
        echo "rpcs3-capture: pixel-inspection FAILED" >&2
        exit 1
    fi
    echo "rpcs3-capture: pixel-inspection PASSED"
elif [[ -n "${CHECK_X:-}" && -n "${CHECKPOINT_FILE:-}" ]] ; then
    :  # legacy single-checkpoint path removed — use checkpoints:<file>
fi

# ── final verdict ──────────────────────────────────────────────────
if (( FATAL )); then
    echo "rpcs3-capture: fatal RPCS3 log entry found" >&2
    exit 1
fi
echo "rpcs3-capture: PASS"

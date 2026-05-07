#!/usr/bin/env bash
# perf_baseline.sh — Phase 0 performance baseline capture
#
# Runs hello-spu.fake.self N=5 times under the from-source RPCS3 build
# (--no-gui), captures per-run perf counters from RPCS3.log, and writes
# a summary to baseline.json (never overwrites an existing baseline).
#
# Cold/warm separation:
#   - Run 1: LLVM cache cleared first → 'cold_run' metrics in baseline.json
#   - Runs 2-5: warm cache → 'warm_runs' median/min/max in baseline.json
#
# Metrics captured per run:
#   - wall_sec: total elapsed wall time
#   - llvm_compile_count: number of 'PPU: LLVM: Compiling module' events
#   - syscall_stats_count: number of 'PPU Syscall Usage Stats:' blocks
#   - exit_code: RPCS3 process exit code

set -euo pipefail

# --- paths -----------------------------------------------------------
RPCS3_BIN="/home/firebirdta01/source/repos/RPCS3/build/bin/rpcs3"
SAMPLE_SELF="/home/firebirdta01/source/repos/PS3_Custom_Toolchain/samples/spu/hello-spu/hello-spu.fake.self"
LOG_FILE="${HOME}/.cache/rpcs3/RPCS3.log"
LLVM_CACHE="${HOME}/.cache/rpcs3/cache"
OUT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASELINE_FILE="${OUT_DIR}/baseline.json"
TIMEOUT=30
COLD_TIMEOUT=60
N_RUNS=5

# --- pre-flight ------------------------------------------------------
if [[ ! -x "$RPCS3_BIN" ]]; then
    echo "ERROR: rpcs3 binary not found at $RPCS3_BIN" >&2; exit 1
fi
if [[ ! -f "$SAMPLE_SELF" ]]; then
    echo "ERROR: sample .fake.self not found at $SAMPLE_SELF" >&2; exit 1
fi

# If baseline.json already exists, rename to dated version
if [[ -f "$BASELINE_FILE" ]]; then
    TS=$(date +%Y%m%d-%H%M%S)
    MV_TARGET="${OUT_DIR}/baseline-${TS}.json"
    mv "$BASELINE_FILE" "$MV_TARGET"
    echo "Existing baseline moved to $MV_TARGET"
fi

# --- helpers ---------------------------------------------------------
median() {
    local -a sorted=($(printf '%s\n' "$@" | sort -n))
    local len=${#sorted[@]}
    if (( len % 2 )); then
        echo "${sorted[$((len / 2))]}"
    else
        local a="${sorted[$((len / 2 - 1))]}"
        local b="${sorted[$((len / 2))]}"
        awk "BEGIN { printf \"%.3f\", ($a + $b) / 2.0 }"
    fi
}
min() { printf '%s\n' "$@" | sort -n | head -1; }
max() { printf '%s\n' "$@" | sort -n | tail -1; }

run_once() {
    # $1 = run label (for display only)
    local label="$1"; shift || true

    rm -f "$LOG_FILE"

    START_NS=$(date +%s.%N 2>/dev/null || echo 0)

    set +e  # timeout may return 124 — don't abort the script
    timeout "$TIMEOUT" "$RPCS3_BIN" --no-gui "$SAMPLE_SELF" >/dev/null 2>&1
    local exit_code=$?
    set -e

    END_NS=$(date +%s.%N 2>/dev/null || echo 0)

    if [[ ! -f "$LOG_FILE" ]]; then
        echo "WARN: RPCS3.log missing for run '${label}' — skipping" >&2
        return 1
    fi

    local wall
    wall=$(awk "BEGIN { printf \"%.3f\", $END_NS - $START_NS }" 2>/dev/null || echo 0)

    local llvm_count
    llvm_count=$(grep -c 'PPU: LLVM: Compiling module' "$LOG_FILE" 2>/dev/null; true)
    llvm_count=$(echo -n "${llvm_count:-0}")

    local syscall_count
    syscall_count=$(grep -c 'PPU Syscall Usage Stats:' "$LOG_FILE" 2>/dev/null; true)
    syscall_count=$(echo -n "${syscall_count:-0}")

    echo "  ${label}: wall=${wall}s  llvm_compile=${llvm_count}  syscall_stats=${syscall_count}  exit=${exit_code}"

    # Return values via global-ish output variables
    echo "${wall}" >&3
    echo "${llvm_count}" >&4
    echo "${syscall_count}" >&5
    echo "${exit_code}" >&6
}

# --- cold run (wipe LLVM cache first) --------------------------------
echo "=== Cold run (LLVM cache wiped) ==="
rm -rf "$LLVM_CACHE"

# Use fd trick to capture the four return values from run_once
exec 3>&1 4>&1 5>&1 6>&1
COLD_WALL=$( { run_once "cold" 3>&1 1>&3 4>&1 1>&4 5>&1 1>&5 6>&1 1>&6; } 2>&1 | tail -4)
# ... this fd dance is fragile. Simpler: use temp files.

# Actually, let's just use a subshell with known output format
rm -f "$LOG_FILE"
rm -rf "$LLVM_CACHE"

echo "--- Cold run ---"
rm -f "$LOG_FILE"
START_NS=$(date +%s.%N 2>/dev/null || echo 0)
set +e
timeout "$COLD_TIMEOUT" "$RPCS3_BIN" --no-gui "$SAMPLE_SELF" >/dev/null 2>&1
COLD_EXIT=$?
set -e
END_NS=$(date +%s.%N 2>/dev/null || echo 0)
COLD_WALL=$(awk "BEGIN { printf \"%.3f\", $END_NS - $START_NS }" 2>/dev/null || echo 0)
COLD_LLVM=$(grep -c 'PPU: LLVM: Compiling module' "$LOG_FILE" 2>/dev/null; true)
COLD_LLVM=$(echo -n "${COLD_LLVM:-0}")
COLD_SYSCALL=$(grep -c 'PPU Syscall Usage Stats:' "$LOG_FILE" 2>/dev/null; true)
COLD_SYSCALL=$(echo -n "${COLD_SYSCALL:-0}")
echo "  cold: wall=${COLD_WALL}s  llvm_compile=${COLD_LLVM}  syscall_stats=${COLD_SYSCALL}  exit=${COLD_EXIT}"
sleep 0.5

# --- warm runs (2-5) ------------------------------------------------
echo ""
echo "=== Warm runs ==="
declare -a WARM_WALLS=()
declare -a WARM_LLVMS=()
declare -a WARM_SYSCALLS=()
declare -a WARM_EXITS=()

for ((run=2; run<=N_RUNS; run++)); do
    rm -f "$LOG_FILE"
    START_NS=$(date +%s.%N 2>/dev/null || echo 0)
    set +e
    timeout "$TIMEOUT" "$RPCS3_BIN" --no-gui "$SAMPLE_SELF" >/dev/null 2>&1
    local_exit=$?
    set -e
    END_NS=$(date +%s.%N 2>/dev/null || echo 0)

    if [[ ! -f "$LOG_FILE" ]]; then
        echo "WARN: RPCS3.log missing for warm run $run — skipping"
        continue
    fi

    WALL=$(awk "BEGIN { printf \"%.3f\", $END_NS - $START_NS }" 2>/dev/null || echo 0)
    LLVM=$(grep -c 'PPU: LLVM: Compiling module' "$LOG_FILE" 2>/dev/null; true)
    LLVM=$(echo -n "${LLVM:-0}")
    SYSCALL=$(grep -c 'PPU Syscall Usage Stats:' "$LOG_FILE" 2>/dev/null; true)
    SYSCALL=$(echo -n "${SYSCALL:-0}")

    WARM_WALLS+=("$WALL")
    WARM_LLVMS+=("$LLVM")
    WARM_SYSCALLS+=("$SYSCALL")
    WARM_EXITS+=("$local_exit")

    echo "  warm${run}: wall=${WALL}s  llvm_compile=${LLVM}  syscall_stats=${SYSCALL}  exit=${local_exit}"
    sleep 0.5
done

# --- warm stats ------------------------------------------------------
WARM_WALL_MED=$(median "${WARM_WALLS[@]}")
WARM_WALL_MIN=$(min "${WARM_WALLS[@]}")
WARM_WALL_MAX=$(max "${WARM_WALLS[@]}")

WARM_LLVM_MED=$(median "${WARM_LLVMS[@]}")
WARM_LLVM_MIN=$(min "${WARM_LLVMS[@]}")
WARM_LLVM_MAX=$(max "${WARM_LLVMS[@]}")

WARM_SYSCALL_MED=$(median "${WARM_SYSCALLS[@]}")
WARM_SYSCALL_MIN=$(min "${WARM_SYSCALLS[@]}")
WARM_SYSCALL_MAX=$(max "${WARM_SYSCALLS[@]}")

# --- git rev ---------------------------------------------------------
GIT_REV=$(cd /home/firebirdta01/source/repos/RPCS3 && git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# --- write baseline.json ---------------------------------------------
cat > "$BASELINE_FILE" << EOF
{
  "phase": 0,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "git_rev": "$GIT_REV",
  "workload": "hello-spu.fake.self",
  "n_runs": $N_RUNS,
  "cold_run": {
    "wall_sec": $COLD_WALL,
    "llvm_compile_count": $COLD_LLVM,
    "syscall_stats_count": $COLD_SYSCALL,
    "exit_code": $COLD_EXIT
  },
  "warm_runs": {
    "wall_sec": {
      "median": $WARM_WALL_MED,
      "min": $WARM_WALL_MIN,
      "max": $WARM_WALL_MAX
    },
    "llvm_compile_count": {
      "median": $WARM_LLVM_MED,
      "min": $WARM_LLVM_MIN,
      "max": $WARM_LLVM_MAX
    },
    "syscall_stats_count": {
      "median": $WARM_SYSCALL_MED,
      "min": $WARM_SYSCALL_MIN,
      "max": $WARM_SYSCALL_MAX
    },
    "raw_exit_codes": [$(IFS=,; echo "${WARM_EXITS[*]}")],
    "raw_wall_times": [$(IFS=,; echo "${WARM_WALLS[*]}")],
    "raw_llvm_counts": [$(IFS=,; echo "${WARM_LLVMS[*]}")],
    "raw_syscall_counts": [$(IFS=,; echo "${WARM_SYSCALLS[*]}")]
  }
}
EOF

echo ""
echo "Baseline written to $BASELINE_FILE"
echo "  cold:   wall=${COLD_WALL}s  llvm=${COLD_LLVM}  syscall=${COLD_SYSCALL}  exit=${COLD_EXIT}"
echo "  warm:   wall_med=${WARM_WALL_MED}s  wall_min=${WARM_WALL_MIN}s  wall_max=${WARM_WALL_MAX}s"
echo "          llvm_med=${WARM_LLVM_MED}  llvm_min=${WARM_LLVM_MIN}  llvm_max=${WARM_LLVM_MAX}"
echo "          syscall_med=${WARM_SYSCALL_MED}  syscall_min=${WARM_SYSCALL_MIN}  syscall_max=${WARM_SYSCALL_MAX}"
echo "  git:     $GIT_REV"

exit 0

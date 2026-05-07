#!/usr/bin/env bash
# smoke_test.sh — Phase 0 regression smoke test for multi-process XMB
#
# Boots hello-spu.fake.self under the from-source RPCS3 build (--no-gui)
# and validates that the spu_thread_printf HLE is bound, formatted output
# appears on the sys_libc_spu_printf log channel, and the SPU exits cleanly.
#
# Exit: 0 on all-pass, 1 on any miss with a one-line diagnostic.

set -euo pipefail

# --- paths -----------------------------------------------------------
RPCS3_BIN="/home/firebirdta01/source/repos/RPCS3/build/bin/rpcs3"
SAMPLE_SELF="/home/firebirdta01/source/repos/PS3_Custom_Toolchain/samples/spu/hello-spu/hello-spu.fake.self"
LOG_FILE="${HOME}/.cache/rpcs3/RPCS3.log"
TIMEOUT=30

# --- pre-flight checks -----------------------------------------------
die() { echo "FAIL: $*" >&2; exit 1; }

if [[ ! -x "$RPCS3_BIN" ]]; then
    die "rpcs3 binary not found at $RPCS3_BIN — rebuild RPCS3 first"
fi
if [[ ! -f "$SAMPLE_SELF" ]]; then
    die "sample .fake.self not found at $SAMPLE_SELF — rebuild hello-spu first"
fi

# Idempotency: clear the log so back-to-back runs give identical results
rm -f "$LOG_FILE"

# --- boot (--no-gui, exit 0 expected) --------------------------------
timeout "$TIMEOUT" "$RPCS3_BIN" --no-gui "$SAMPLE_SELF" >/dev/null 2>&1 &
RPCS3_PID=$!
wait $RPCS3_PID
EXIT_CODE=$?

if [[ ! -f "$LOG_FILE" ]]; then
    die "RPCS3.log was not created — RPCS3 did not boot"
fi

# Exit code must be 0 (clean self-termination); 124 = hung, other = crash
case "$EXIT_CODE" in
    0)   ;; # pass
    124) die "rpcs3 timed out after ${TIMEOUT}s — possible hang"
         ;;
    *)   die "rpcs3 exited with code $EXIT_CODE — possible crash"
         ;;
esac

# --- assertions ------------------------------------------------------
total_misses=0

# 1. HLE bound
if ! grep -q 'sys_libc import: \[spu_thread_printf\] (0xb1f4779d)' "$LOG_FILE"; then
    echo "MISS: spu_thread_printf HLE not bound (expected NID 0xb1f4779d)" >&2
    total_misses=$((total_misses + 1))
fi

# 2. 15 sys_libc_spu_printf success-level lines
printf_count=$(grep -c 'sys_libc_spu_printf:' "$LOG_FILE" || true)
if [[ "$printf_count" -ne 15 ]]; then
    echo "MISS: sys_libc_spu_printf lines expected 15, got $printf_count" >&2
    total_misses=$((total_misses + 1))
fi

# 3. Clean SPU exit
if ! grep -q 'spu finished; cause=2 status=0 done=12648430' "$LOG_FILE"; then
    echo "MISS: SPU did not exit cleanly (expected cause=2 status=0 done=0xc0ffee)" >&2
    total_misses=$((total_misses + 1))
fi

# --- result ----------------------------------------------------------
if [[ "$total_misses" -eq 0 ]]; then
    echo "PASS: smoke_test.sh — all markers present"
    exit 0
else
    die "$total_misses assertion(s) failed"
fi

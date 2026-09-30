#!/usr/bin/env bash
# Host test of the SPU libsync2 waiting queue: sdk/libsync2/src/
# waiting_queue.c is compiled unchanged, twice under different names (two
# SPUs with separate local stores), against the simulated MFC in
# tests/sdk/fixtures/spu-mfc-mock, and driven by
# tests/sdk/sync2-waiting-queue-host-test.c: index wrap and phase flips over
# thousands of enter / wake cycles, FIFO order, and the wake-before-entry
# (E marker) path forced on every early step.
#
# usage: sync2-waiting-queue-host-test.sh     (CC overrides the host compiler)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc="${CC:-cc}"
mock="$root/tests/sdk/fixtures/spu-mfc-mock"
src="${SYNC2_SRC:-$root/sdk/libsync2/src}"   # SYNC2_SRC: a mutated copy, for controls
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

flags=(-std=gnu11 -O2 -Wall -Wextra -Werror -D__SPU__ -DSYNC2_HOST_TEST
       -I"$mock" -I"$root/sdk/include-spu" -idirafter "$root/sdk/include")
for side in A B; do
    rename=(-D__sync2_line=${side}_line -D__sync2_queue_enter=${side}_enter -D__sync2_queue_wakeup=${side}_wakeup)
    "$cc" "${flags[@]}" "${rename[@]}" -c "$src/waiting_queue.c" -o "$work/wq_$side.o"
    "$cc" "${flags[@]}" "${rename[@]}" -c "$src/lock_line.c" -o "$work/line_$side.o"
done
"$cc" "${flags[@]}" "$mock/mfc_mock.c" "$root/tests/sdk/sync2-waiting-queue-host-test.c" \
    "$work"/wq_A.o "$work"/wq_B.o "$work"/line_A.o "$work"/line_B.o -o "$work/sync2-wq"
"$work/sync2-wq"

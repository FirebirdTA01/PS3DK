#!/usr/bin/env bash
# Host test of the SPU libsync queue, rwm, mutex and barrier sources: they
# are compiled unchanged against the simulated MFC in
# tests/sdk/fixtures/spu-mfc-mock and exercised by
# tests/sdk/libsync-spu-host-test.c (descriptor survival, the PPU-compatible
# ticket mutex against a PPU-side model, barrier count validation).
#
# usage: libsync-spu-host-test.sh     (CC overrides the host compiler)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc="${CC:-cc}"
mock="$root/tests/sdk/fixtures/spu-mfc-mock"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -D__SPU__ \
    -I"$mock" -I"$root/sdk/include-spu" -I"$root/sdk/libsync/src" -idirafter "$root/sdk/include" \
    "$root/sdk/libsync/src/queue.c" "$root/sdk/libsync/src/rwm.c" \
    "$root/sdk/libsync/src/mutex.c" "$root/sdk/libsync/src/barrier.c" \
    "$mock/mfc_mock.c" "$root/tests/sdk/libsync-spu-host-test.c" \
    -o "$work/libsync-spu"
"$work/libsync-spu"

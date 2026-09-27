#!/usr/bin/env bash
# libdma's cellDmaLargeCmd and cellDmaUnalignedCmd, compiled on the host with
# a stub <spu_mfcio.h> whose spu_mfcdma64 records each command.  The probe
# (libdma-split-host-test.c) requires every command to be a transfer the MFC
# accepts and the commands to cover the range exactly.  The unaligned
# splitter used to issue 3-, 5-, 6-, 7- and 9..15-byte heads and tails.
#
# usage: libdma-split-host-test.sh [--src DIR]
#   --src  libdma source directory (default: this tree's sdk/libdma/src).
#          Point it at an older tree's sources to reproduce the failure.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
src="$root/sdk/libdma/src"
while [ $# -gt 0 ]; do
    case "$1" in
        --src) src="$2"; shift 2 ;;
        *) echo "usage: $0 [--src DIR]" >&2; exit 2 ;;
    esac
done
cc=${CC:-cc}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/spu_mfcio.h" <<'EOF'
#ifndef STUB_SPU_MFCIO_H
#define STUB_SPU_MFCIO_H
void spu_mfcdma64_record(void *ls, unsigned int eah, unsigned int eal,
                         unsigned int size, unsigned int tag, unsigned int cmd);
#define spu_mfcdma64(ls, eah, eal, size, tag, cmd) \
    spu_mfcdma64_record((ls), (eah), (eal), (size), (tag), (cmd))
#endif
EOF
if ! "$cc" -std=c99 -Wall -Wextra -Werror -I"$work" \
        "$root/tests/sdk/libdma-split-host-test.c" \
        "$src/dma_large_cmd.c" "$src/dma_unaligned_cmd.c" \
        -o "$work/probe" > "$work/build.log" 2>&1; then
    echo "libdma-split: FAIL: host build"
    sed 's/^/    /' "$work/build.log" | head -20
    exit 1
fi
"$work/probe"

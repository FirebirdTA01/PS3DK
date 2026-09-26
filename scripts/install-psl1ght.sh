#!/usr/bin/env bash
# Install PSL1GHT without replacing SDK-owned pointer-width-safe PPU headers.
set -euo pipefail
if [[ $# -ne 1 ]]; then
    printf 'Usage: %s PSL1GHT_SOURCE\n' "$0" >&2
    exit 2
fi
src=$1
: "${PSL1GHT:?PSL1GHT must name the install root}"

# Exact paths only: every other sys/ and lv2/ header remains upstream-owned.
owned=(sys/mutex.h sys/cond.h sys/event_queue.h sys/sem.h sys/systime.h sys/spu.h
       sys/thread.h sys/process.h sys/synchronization.h sys/lv2_fs_ext.h ppu-asm.h
       sys/prx.h lv2/prx.h)
excludes=()
for header in "${owned[@]}"; do excludes+=("--exclude=./$header"); done
mkdir -p "$PSL1GHT/ppu/include"
tar -C "$src/ppu/include" "${excludes[@]}" -cf - . |
    tar -C "$PSL1GHT/ppu/include" -xf -

# Preserve upstream CRT, archive, SPU, common-header and host-tool dispatch.
make -C "$src/ppu" -o install-headers install
for component in spu common tools; do
    make -C "$src/$component" install
done

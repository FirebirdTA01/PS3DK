#!/usr/bin/env bash
# Combine an unchanged nidgen sys_net import archive with the SDK front end.
# Usage: build-libnet-archive.sh RAW_ARCHIVE OUTPUT_ARCHIVE ilp32|lp64
# PS3DEV and PS3DK must designate the intended install prefix.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd -P)
raw=$1
target=$2
abi=$3
flags=()
case "$abi" in ilp32) ;; lp64) flags=(-mlp64) ;; *) exit 2 ;; esac
: "${PS3DEV:?}" "${PS3DK:?}"
work=${PS3_BUILD_ROOT:-$root/build}/libnet/$abi
mkdir -p "$work" "$(dirname "$target")"
objects=()
for src in "$root"/sdk/libnet/src/*.c; do
    obj="$work/$(basename "${src%.c}").o"
    "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" "${flags[@]}" -mcpu=cell -std=c11 \
        -O2 -Wall -Wextra -Werror -I"$PS3DK/ppu/include" -c "$src" -o "$obj"
    objects+=("$obj")
done
# Only symbol-table names change. Import records, FNIDs, header, anchors and
# trampoline instructions stay byte-identical to nidgen's output.
candidate=$(mktemp "$(dirname "$target")/.libnet-$abi.XXXXXX.a")
trap 'rm -f -- "$candidate"' EXIT
"$PS3DEV/ppu/bin/powerpc64-ps3-elf-objcopy" \
    --redefine-syms="$root/sdk/libnet/raw-symbols.txt" "$raw" "$candidate"
"$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$candidate" "${objects[@]}"
"$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$candidate"
chmod 644 "$candidate"
mv -f -- "$candidate" "$target"

#!/usr/bin/env bash
# <cell/fs/cell_fs_file_api.h> record layouts (see cellfs-struct-layout-test.c):
# compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
# under -Wall -Wextra -Werror; the sizeof/offsetof/alignof asserts run in
# every mode.
# Self-checks: forcing 4-byte packing (-fpack-struct=4, the old layout) must
# fail the CellFsAio offset asserts, and a header whose AIO buffer is a plain
# pointer must fail the buffer-width assert under -mlp64.
#
# usage: cellfs-struct-layout-test.sh [--ps3dev DIR] [--include DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "cellfs-struct-layout: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "cellfs-struct-layout: FAIL: no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/cellfs-struct-layout-test.c"
status=0

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$inc" -c "$src" -o "$work/p.o" 2>&1); then
                echo "cellfs-struct-layout: ok   compile $label"
            else
                echo "cellfs-struct-layout: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# selfcheck <label> <expected message> <compiler args...>: the compile must
# fail, and with that assert message.
selfcheck() {
    local label=$1 expect=$2; shift 2
    if "$cc" -x c -std=c11 "$@" -c "$src" -o "$work/bad.o" > "$work/self.txt" 2>&1; then
        echo "cellfs-struct-layout: FAIL self-check: $label was not caught"; status=1
    elif grep -qF "$expect" "$work/self.txt"; then
        echo "cellfs-struct-layout: ok   self-check: $label is caught"
    else
        echo "cellfs-struct-layout: FAIL self-check $label failed for the wrong reason"
        sed 's/^/    /' "$work/self.txt" | head -5; status=1
    fi
}
for abi in "" -mlp64; do
    selfcheck "4-byte packing (${abi:-ilp32})" "CellFsAio.offset offset" $abi -fpack-struct=4 -I"$inc"
done
mkdir -p "$work/ov/cell/fs"
sed 's/void    \*buf ATTRIBUTE_PRXPTR;/void    *buf;/' "$inc/cell/fs/cell_fs_file_api.h" > "$work/ov/cell/fs/cell_fs_file_api.h"
if cmp -s "$inc/cell/fs/cell_fs_file_api.h" "$work/ov/cell/fs/cell_fs_file_api.h"; then
    echo "cellfs-struct-layout: FAIL self-check could not plant a plain buffer pointer"; status=1
else
    selfcheck "a plain AIO buffer pointer (lp64)" "CellFsAio.buf width" -mlp64 -I"$work/ov" -I"$inc"
fi

[ $status -eq 0 ] && echo "PASS: cellfs-struct-layout-test"
exit $status

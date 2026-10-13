#!/usr/bin/env bash
# <np/drm_inline.h> (np-drm-inline-test.c): the sceNpDrmOpen inline shim.
#  1. compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror; the test TU calls sceNpDrmOpen, so the
#     inline body is compiled in every mode;
#  2. with an installed SDK, link the ILP32 object against libsysutil_np_stub
#     and libfs_stub: cellFsStat, sceNpDrmIsAvailable and cellFsOpen resolve.
# Self-check: a copy of the header with cellFsOpen misspelled must fail.
#
# usage: np-drm-inline-test.sh [--ps3dev DIR] [--include DIR]
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
    echo "np-drm-inline: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "np-drm-inline: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/np-drm-inline-test.c"
status=0

# Only the new header goes in front of the installed SDK.
mkdir -p "$work/ov/np"
cp "$inc/np/drm_inline.h" "$work/ov/np/"
LIBS="-lsysutil_np_stub -lfs_stub"

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "np-drm-inline: ok   compile $label"
            else
                echo "np-drm-inline: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# The link needs the stub archives of an installed SDK; a toolchain alone
# (CI without the SDK build) checks the header only.
if [ ! -f "$sdk/ppu/lib/libsysutil_np_stub.a" ] || [ ! -f "$sdk/ppu/lib/libfs_stub.a" ]; then
    echo "np-drm-inline: SKIP link and self-check (no stub archives under $sdk/ppu/lib)"
    [ $status -eq 0 ] && echo "PASS: np-drm-inline-test"
    exit $status
fi
if "$cc" -std=gnu99 -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/l.o" 2>"$work/l.err" &&
   "$cc" "$work/l.o" -o "$work/l.elf" -L"$sdk/ppu/lib" $LIBS 2>>"$work/l.err"; then
    echo "np-drm-inline: ok   link: cellFsStat, sceNpDrmIsAvailable and cellFsOpen resolve"
else
    echo "np-drm-inline: FAIL link"; grep -E "undefined|error" "$work/l.err" | sed 's/^/    /' | head -10
    status=1
fi

# Self-check: misspell cellFsOpen in a copy of the header; the body then
# calls an undeclared function, which -Werror refuses.
mkdir -p "$work/bad"; cp -r "$work/ov/." "$work/bad/"
sed 's/cellFsOpen(/cellFsOpeX(/' "$inc/np/drm_inline.h" > "$work/bad/np/drm_inline.h"
if cmp -s "$inc/np/drm_inline.h" "$work/bad/np/drm_inline.h"; then
    echo "np-drm-inline: FAIL self-check could not plant the misspelling"; status=1
elif ! "$cc" -std=gnu99 -Wall -Wextra -Werror -I"$work/bad" -I"$sdk/ppu/include" -c "$src" -o "$work/b.o" > "$work/b.err" 2>&1 &&
     grep -q "cellFsOpeX" "$work/b.err"; then
    echo "np-drm-inline: ok   self-check: a misspelled call in the body fails the build"
else
    echo "np-drm-inline: FAIL self-check: the misspelled call was not caught"; status=1
fi

[ $status -eq 0 ] && echo "PASS: np-drm-inline-test"
exit $status

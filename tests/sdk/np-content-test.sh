#!/usr/bin/env bash
# The NP names added to <cell/np/common.h> and <cell/np/tus.h> and the
# <np/common.h>, <np/ticket.h>, <np/tustorage.h> paths (np-content-test.c):
#  1. compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror (the value asserts run in every mode);
#  2. with an installed SDK, link against the TUS stub archive: the TSS
#     context names must expand to its exports.
# Self-check: a TSS alias pointed at a name the archive does not export must
# fail the link; the real header must link (the control).
#
# usage: np-content-test.sh [--ps3dev DIR] [--include DIR]
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
    echo "np-content: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "np-content: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/np-content-test.c"
status=0

# Only the headers this change adds or edits go in front of the installed SDK.
mkdir -p "$work/ov/cell/np" "$work/ov/np"
cp "$inc/cell/np/common.h" "$inc/cell/np/tus.h" "$work/ov/cell/np/"
cp "$inc/np/common.h" "$inc/np/ticket.h" "$inc/np/tustorage.h" "$work/ov/np/"
LIBS="-lsysutil_np_tus_stub"

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "np-content: ok   compile $label"
            else
                echo "np-content: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# The link needs the stub archives of an installed SDK; a toolchain alone
# (CI without the SDK build) checks the headers only.
if [ ! -f "$sdk/ppu/lib/libsysutil_np_tus_stub.a" ]; then
    echo "np-content: SKIP link and self-check (no stub archives under $sdk/ppu/lib)"
    [ $status -eq 0 ] && echo "PASS: np-content-test"
    exit $status
fi
if "$cc" -std=gnu99 -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/l.o" 2>"$work/l.err" &&
   "$cc" "$work/l.o" -o "$work/l.elf" -L"$sdk/ppu/lib" $LIBS 2>>"$work/l.err"; then
    echo "np-content: ok   link: the TSS context names resolve to TUS exports"
else
    echo "np-content: FAIL link"; grep -E "undefined|error" "$work/l.err" | sed 's/^/    /' | head -10
    status=1
fi

# Self-check: point one TSS alias at a name no archive exports.
mkdir -p "$work/bad"; cp -r "$work/ov/." "$work/bad/"
sed 's/sceNpTusDestroyTitleCtx(titleCtxId)$/sceNpTusDestroyTitleCtxX(titleCtxId)/' "$inc/cell/np/tus.h" > "$work/bad/cell/np/tus.h"
if cmp -s "$inc/cell/np/tus.h" "$work/bad/cell/np/tus.h"; then
    echo "np-content: FAIL self-check could not plant the bad alias"; status=1
elif "$cc" -std=gnu99 -I"$work/bad" -I"$sdk/ppu/include" -c "$src" -o "$work/b.o" 2>/dev/null &&
     ! "$cc" "$work/b.o" -o "$work/b.elf" -L"$sdk/ppu/lib" $LIBS > "$work/b.err" 2>&1 &&
     grep -q "sceNpTusDestroyTitleCtxX" "$work/b.err"; then
    echo "np-content: ok   self-check: an alias to a missing export fails the link"
else
    echo "np-content: FAIL self-check: a bad alias was not caught"; status=1
fi

[ $status -eq 0 ] && echo "PASS: np-content-test"
exit $status

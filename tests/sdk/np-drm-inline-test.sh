#!/usr/bin/env bash
# tests/sdk/np-drm-inline-test.sh
#
# Compile and (if stub archives exist) link the <np/drm_inline.h> inline
# shim under the full C / C++17 / ILP32 / LP64 matrix.
#
# Skips (exit 0) when no PPU toolchain is available (PS3DEV unset).
# Uses the INSTALLED SDK include path (overlay in front of $sdk/ppu/include),
# never puts the source-tree $sdk/include directly on the path.
#
# Matrix: gnu99, c11, c++17, each with and without -mlp64.
# -Wall -Wextra -Werror throughout.
#
# Self-check: a bad copy of the forwarder with cellFsOpen misspelled must
# fail to compile (the inline body really references that symbol).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
inc="$repo/sdk/include"
ref="$here/np-drm-inline-test.c"
[ -f "$ref" ] || { echo "ref TU $ref not found" >&2; exit 1; }

# --- PS3DEV gate: skip if no toolchain ---
PS3DEV="${PS3DEV:-}"
if [ -z "$PS3DEV" ]; then
    echo "SKIP — PS3DEV not set (no PPU toolchain)"
    exit 0
fi
sdk="$PS3DEV/ps3dk"
[ -d "$sdk/ppu/include" ] || { echo "SKIP — $sdk/ppu/include not found"; exit 0; }

CC="${CC:-powerpc64-ps3-elf-gcc}"
CXX="${CXX:-powerpc64-ps3-elf-g++}"
for tool in "$CC" "$CXX"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP — $tool not on PATH"; exit 0; }
done

ov="$(mktemp -d)"
ovb="$(mktemp -d)"
trap 'rm -rf "$ov" "$ovb"' EXIT

fail=0
run_ok()  { local label="$1"; shift; if "$@" >/dev/null 2>"$ov/e"; then :; else echo "FAIL(ok)  $label"; sed -n '1,14p' "$ov/e" 2>/dev/null; fail=1; fi; }
run_bad() { local label="$1"; shift; if "$@" >/dev/null 2>&1; then echo "FAIL(bad) $label compiled when it should not"; fail=1; fi; }

# ----------------------------------------------------------------
# Overlay: the inline header in front of the INSTALLED SDK
# ----------------------------------------------------------------
mkdir -p "$ov/np"
cp "$inc/np/drm_inline.h" "$ov/np/drm_inline.h"
ovinc="-I$ov -I$sdk/ppu/include"

# ----------------------------------------------------------------
# 1) Matrix compile: gnu99, c11 (C) — c++17 (C++)
#    each with and without -mlp64, -Wall -Wextra -Werror
# ----------------------------------------------------------------
for std in gnu99 c11; do
    run_ok "C $std"      "$CC"  -x c   -std="$std" $ovinc -Wall -Wextra -Werror -c "$ref" -o "$ov/ref.o"
    run_ok "C $std lp64" "$CC"  -x c   -std="$std" $ovinc -mlp64 -Wall -Wextra -Werror -c "$ref" -o "$ov/ref.o"
done
run_ok "C++ c++17"      "$CXX" -x c++ -std=c++17 $ovinc -Wall -Wextra -Werror -c "$ref" -o "$ov/ref.o"
run_ok "C++ c++17 lp64" "$CXX" -x c++ -std=c++17 $ovinc -mlp64 -Wall -Wextra -Werror -c "$ref" -o "$ov/ref.o"

# ----------------------------------------------------------------
# 2) Link the ILP32 object (only when the stub archives exist)
#    -lsysutil_np_stub provides sceNpDrmIsAvailable; -lfs_stub provides cellFsStat / cellFsOpen.
# ----------------------------------------------------------------
libdir="$sdk/ppu/lib"
if [ -f "$libdir/sysutil_np_stub.a" ] && [ -f "$libdir/fs_stub.a" ]; then
    "$CC" -x c -std=gnu99 $ovinc -Wall -Wextra -Werror \
          -c "$ref" -o "$ov/link.o" 2> "$ov/e"
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL(ok) compile for link"; sed -n '1,14p' "$ov/e" 2>/dev/null; fail=1
    else
        "$CC" "$ov/link.o" -O2 -mcpu=cell -L"$libdir" -lsysutil_np_stub -lfs_stub \
              -o "$ov/link.bin" 2> "$ov/e"
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "FAIL(link)  link with stubs"; sed -n '1,14p' "$ov/e" 2>/dev/null; fail=1
        else
            echo "link OK"
        fi
    fi
else
    echo "SKIP link: sysutil_np_stub.a or fs_stub.a not in $libdir/"
fi

# ----------------------------------------------------------------
# 3) Self-check: a bad copy of the inline header (cellFsOpen misspelled)
#    must fail to compile. The bad TU CALLS sceNpDrmOpen so the inline body
#    is really instantiated and the (now misspelled) cellFsOpeX reference is
#    emitted - an undeclared symbol under -Werror -Wimplicit-function-declaration.
#    This proves the body is compiled rather than silently dropped.
# ----------------------------------------------------------------
mkdir -p "$ovb/np"
sed 's/cellFsOpen/cellFsOpeX/g' "$inc/np/drm_inline.h" > "$ovb/np/drm_inline.h"
printf '%s\n' '#include <np/drm_inline.h>' 'int bad_probe(const char *p, int f)' '{' 'int fd; return sceNpDrmOpen(0, p, f, &fd, 0, 0);' '}' > "$ovb/bad.c"
run_bad  "self-check: misspelled cellFsOpen breaks the build" \
    "$CC" -x c -std=gnu99 -I"$ovb" -I"$sdk/ppu/include" -Wall -Wextra -Werror -c "$ovb/bad.c" -o "$ovb/bad.o"

if [ "$fail" -ne 0 ]; then
    echo "FAIL: one or more checks failed" >&2
    exit 1
fi
echo "PASS: 6 matrix compiles + link (if stubs) + self-check"

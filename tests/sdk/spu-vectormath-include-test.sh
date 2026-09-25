#!/usr/bin/env bash
# SPU code can include the C++ vector math headers.  They include
# <simdmath.h> and use uintptr_t in their load/store helpers; the SDK must
# install its <simdmath.h> forwarder for SPU too, and the forwarder brings in
# <stdint.h> (the reference SPU vector math includes it itself).  The PPU
# side must keep working through the same forwarder.
#
# usage: spu-vectormath-include-test.sh [--ps3dev DIR]
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spu-vectormath-include: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
sdk="${PS3DK:-$ps3dev/ps3dk}"
spu="$ps3dev/spu/bin/spu-elf-g++"
ppu="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
status=0
fail() { echo "spu-vectormath-include: FAIL: $*"; status=1; }
ok() { echo "spu-vectormath-include: ok   $*"; }
[ -x "$spu" ] && [ -x "$ppu" ] || { fail "no compilers under $ps3dev"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

[ -f "$sdk/spu/include/simdmath.h" ] && ok "SPU tree installs <simdmath.h>" \
    || fail "SPU tree has no <simdmath.h>"

cat > "$work/v.cpp" <<'EOF'
#include <vectormath/cpp/vectormath_aos.h>
using namespace Vectormath::Aos;
float dot3(Vector3 a, Vector3 b) { return dot(a, b); }
void store(float *p, Vector3 v) { storeXYZ(v, p); }
Matrix4 mul(Matrix4 a, Matrix4 b) { return a * b; }
EOF
for std in -std=c++98 -std=c++17; do
    if "$spu" $std -I"$sdk/spu/include" -O2 -Wall -Werror -c "$work/v.cpp" -o "$work/v.o" > "$work/e.log" 2>&1; then
        ok "SPU C++ vector math compiles ($std)"
    else
        fail "SPU C++ vector math ($std): $(grep -m1 -E 'error' "$work/e.log")"
    fi
done

printf '#include <simdmath.h>\nvector float f(vector float a) { return sinf4(a); }\n' > "$work/p.c"
for abi in "" -mlp64; do
    if "$ppu" $abi -I"$sdk/ppu/include" -O2 -Wall -Werror -c "$work/p.c" -o "$work/p.o" > "$work/e.log" 2>&1; then
        ok "PPU <simdmath.h> still compiles (${abi:-ilp32})"
    else
        fail "PPU <simdmath.h> (${abi:-ilp32}): $(grep -m1 -E 'error' "$work/e.log")"
    fi
done

[ "$status" -eq 0 ] && echo "spu-vectormath-include: PASS"
exit $status

#!/usr/bin/env bash
# SPU C++ front end, -fparen-vector-literals (GCC patch 0014): the literal,
# splat, nested-cast and vector-cast forms compile; a non-arithmetic operand
# keeps the ordinary cast meaning with postfix operators, so (V)(arr)[i],
# (V)(s).m, (V)(f)(x) and (V)(p)->m compile, while (V)(v)[i] casts the
# element and is refused (as the reference compiler refuses it).  Control:
# with -fno-paren-vector-literals the splat is refused again.
# Run-time lane checks for the same forms live in the spurs-suite row
# spu-vector-literals.
set -euo pipefail
ps3dev=${PS3DEV:-}
if [[ -z $ps3dev ]]; then
    echo 'spu-paren-vector-cpp: SKIP (set PS3DEV)'
    exit 0
fi
cxx="$ps3dev/spu/bin/spu-elf-g++"
[[ -x $cxx ]] || { echo "missing tool: $cxx" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat > "$work/good.cpp" <<'SRC'
#include <spu_intrinsics.h>
typedef vector unsigned int V;
typedef vector float F;
struct S { V m; };
V arr[2];
static V make(unsigned x) { return spu_splats(x); }
V splat(unsigned x) { return (V)(x); }
V list(unsigned x) { return (V)(x, x + 1, x + 2, x + 3); }
V nested(float f) { return (V)((unsigned)f); }
V vcast(F f) { return (V)(f); }
V arr_index(int i) { return (V)(arr)[i]; }
V member(S s) { return (V)(s).m; }
V call(unsigned x) { return (V)(make)(x); }
V arrow(S *p) { return (V)(p)->m; }
SRC
cat > "$work/index.cpp" <<'SRC'
#include <spu_intrinsics.h>
typedef vector unsigned int V;
V cast_index(V v, int i) { return (V)(v)[i]; }
SRC

fail=0
expect() { # want_status name output-file command...
    local want=$1 name=$2 log=$3; shift 3
    local got=0
    "$@" > "$log" 2>&1 || got=$?
    if [[ $got -ne $want ]]; then
        echo "FAIL $name: exit $got, want $want"; sed 's/^/  /' "$log"; fail=1
    else
        echo "ok   $name (exit $got)"
    fi
}
expect 0 "literal/splat/cast/postfix forms" "$work/good.log" "$cxx" -O2 -S -o "$work/good.s" "$work/good.cpp"
expect 1 "(V)(v)[i] casts the element" "$work/index.log" "$cxx" -O2 -S -o "$work/index.s" "$work/index.cpp"
if ! grep -q "can't convert a value of type 'unsigned int' to vector type" "$work/index.log"; then
    echo "FAIL (V)(v)[i]: not refused for the element-to-vector size"; fail=1
fi
expect 1 "control: -fno-paren-vector-literals" "$work/control.log" \
    "$cxx" -O2 -fno-paren-vector-literals -S -o "$work/control.s" "$work/good.cpp"
if ((fail)); then echo 'spu-paren-vector-cpp: FAIL'; exit 1; fi
echo 'spu-paren-vector-cpp: PASS'

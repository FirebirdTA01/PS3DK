#!/usr/bin/env bash
# The PPU compiler treats vector, pixel and bool as context-sensitive AltiVec
# keywords under a STRICT -std= too, as the reference compiler does (measured
# on ppu-lv2-gcc 475: `#define vector vector` with __STRICT_ANSI__ for -ansi,
# -std=c99 and -std=c++98).  Upstream GCC drops them when flag_iso is set, so
# the reference samples' shared libraries (gcmutil, fw), compiled with
# -std=c++98, failed in simdmath.h with "'vector' does not name a type".
#
# Rows: the keyword macros in every strict and GNU mode, a vector-typed
# function in each strict mode, identifiers named vector/pixel still usable in
# strict C, std::vector and bool beside AltiVec vectors in strict C++17, and
# the SDK's simdmath/vectormath headers in strict C++.
# usage: altivec-strict-std-test.sh [--ps3dev DIR] [-B DIR]
#   -B DIR is passed to the driver (a candidate cc1/cc1plus directory).
set -u
ps3dev="${PS3DEV:-}"; bdir=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        -B) bdir="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [-B DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "altivec-strict-std: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
inc="$ps3dev/ps3dk/ppu/include"
for t in "$cc" "$cxx"; do
    [ -x "$t" ] || { echo "altivec-strict-std: FAIL: no compiler at $t" >&2; exit 1; }
done
B=(); [ -n "$bdir" ] && B=("-B$bdir")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "altivec-strict-std: FAIL: $*" >&2; status=1; }
ok() { echo "altivec-strict-std: ok   $*"; }

# 1. keyword macros, strict and GNU modes, C and C++, both ABIs.
: > "$work/empty.c"; : > "$work/empty.cpp"
for abi in "" -mlp64; do
    for mode in "$cc -ansi" "$cc -std=c99" "$cc -std=c11" "$cc -std=gnu99" \
                "$cxx -std=c++98" "$cxx -std=c++17" "$cxx -std=gnu++17"; do
        set -- $mode
        src="$work/empty.c"; [ "$1" = "$cxx" ] && src="$work/empty.cpp"
        label="${1##*-} $2 ${abi:-ilp32}"
        # The macro list only counts if the preprocessor itself succeeded.
        if ! defs=$("$1" "${B[@]}" $abi "$2" -E -dM "$src" 2>&1); then
            fail "$label: preprocessor failed: $(printf '%s' "$defs" | head -1)"
            continue
        fi
        missing=""
        for k in vector pixel bool _Bool; do
            printf '%s\n' "$defs" | grep -qx "#define $k $k" || missing="$missing $k"
        done
        [ -z "$missing" ] && ok "$label defines the keywords" || fail "$label lacks:$missing"
    done
done

# 2. strict modes compile vector types and intrinsics.
cat > "$work/vec.c" <<'EOF'
#include <altivec.h>
vector float twice(vector float v) { return vec_add(v, v); }
vector unsigned int ones(void) { return vec_splat_u32(1); }
EOF
cp "$work/vec.c" "$work/vec.cpp"
for mode in "$cc -ansi vec.c" "$cc -std=c99 vec.c" "$cxx -std=c++98 vec.cpp" "$cxx -std=c++17 vec.cpp"; do
    set -- $mode
    if out=$("$1" "${B[@]}" "$2" -Wall -Werror -c "$work/$3" -o "$work/v.o" 2>&1); then
        ok "${1##*-} $2 compiles vector types"
    else
        fail "${1##*-} $2 rejects vector types: $(printf '%s' "$out" | head -2)"
    fi
done

# 3. the keywords are context-sensitive: ordinary identifiers survive.
cat > "$work/ident.c" <<'EOF'
int f(void) { int vector = 3, pixel = 2; _Bool b = 1; return vector + pixel + b; }
EOF
"$cc" "${B[@]}" -std=c99 -Wall -Werror -c "$work/ident.c" -o "$work/i.o" 2>"$work/i.log" \
    && ok "strict C keeps identifiers named vector/pixel" \
    || fail "strict C broke identifiers: $(head -2 "$work/i.log")"
cat > "$work/std.cpp" <<'EOF'
#include <altivec.h>
#include <vector>
std::vector<int> v(3, 1);
bool flag = true;
vector signed int four() { return vec_splat_s32(4); }
int total() { int s = 0; for (int x : v) s += x; return flag ? s : 0; }
EOF
"$cxx" "${B[@]}" -std=c++17 -Wall -Werror -c "$work/std.cpp" -o "$work/s.o" 2>"$work/s.log" \
    && ok "strict C++17 keeps std::vector and bool beside AltiVec vectors" \
    || fail "strict C++17 std::vector/bool: $(head -2 "$work/s.log")"

# 4. the SDK vector headers in strict C++ (the reference samples' mode).
printf '#include <simdmath/simdmath.h>\n#include <vectormath/cpp/vectormath_aos.h>\nVectormath::Aos::Vector3 up() { return Vectormath::Aos::Vector3::yAxis(); }\n' > "$work/sdk.cpp"
for std in c++98 c++17; do
    "$cxx" "${B[@]}" -std=$std -I"$inc" -c "$work/sdk.cpp" -o "$work/sdk.o" 2>"$work/sdk.log" \
        && ok "-std=$std compiles simdmath + vectormath_aos" \
        || fail "-std=$std SDK vector headers: $(head -2 "$work/sdk.log")"
done

[ "$status" -eq 0 ] && echo "altivec-strict-std: PASS"
exit $status

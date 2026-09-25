#!/usr/bin/env bash
# The PPU and SPU compilers predefine the reference PS3 platform macros
# (GCC PPU patch 0046, SPU patch 0008).  Reference headers and samples pick
# their PS3 code paths on __CELLOS_LV2__; without it a sample framework took
# its host branch.  Each row checks the macro's value and, where the data
# model decides, that the other model's macros are absent.
#
# usage: platform-predefines-test.sh [--ps3dev DIR] [--ppu-flag F]... [--spu-flag F]...
#   extra flags are passed to every compile of that processor (for example
#   -specs=FILE to try a spec change against an installed driver).
set -u
ps3dev="${PS3DEV:-}"; ppu_extra=(); spu_extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --ppu-flag) ppu_extra+=("$2"); shift 2 ;;
        --spu-flag) spu_extra+=("$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--ppu-flag F]... [--spu-flag F]..." >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "platform-predefines: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
ppu="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
spu="$ps3dev/spu/bin/spu-elf-gcc"
status=0
fail() { echo "platform-predefines: FAIL: $*"; status=1; }
ok() { echo "platform-predefines: ok   $*"; }
[ -x "$ppu" ] && [ -x "$spu" ] || { fail "no PPU/SPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/empty.c"

# check <label> <want-list "NAME=VALUE ..."> <absent-list "NAME ..."> <compiler> <flags...>
check() {
    local label="$1" want="$2" absent="$3"; shift 3
    if ! "$@" -dM -E "$work/empty.c" > "$work/m.txt" 2> "$work/m.err"; then
        fail "$label: preprocessor failed: $(head -1 "$work/m.err")"; return
    fi
    local kv name value got bad=""
    for kv in $want; do
        name="${kv%%=*}"; value="${kv#*=}"
        got=$(sed -n "s/^#define $name \(.*\)$/\1/p" "$work/m.txt")
        [ "$got" = "$value" ] || bad="$bad $name=${got:-<undefined>}"
    done
    for name in $absent; do
        grep -q "^#define $name " "$work/m.txt" && bad="$bad $name(should be absent)"
    done
    if [ -z "$bad" ]; then ok "$label"; else fail "$label:$bad"; fi
}

common="__CELLOS_LV2__=1 __STRICT_ALIGNED=1"
check "PPU ILP32" "$common __PPU__=1 __LP32__=1 _LP32=1 __POINTER_32BIT__=1" "__LP64__ _LP64" \
    "$ppu" "${ppu_extra[@]}"
check "PPU LP64" "$common __PPU__=1 __LP64__=1 _LP64=1" "__LP32__ _LP32 __POINTER_32BIT__" \
    "$ppu" "${ppu_extra[@]}" -mlp64
check "SPU" "$common __SPU__=1 __LP32__=1 _LP32=1 __DOUBLE_ACCURATE__=1 __FLOAT_FAST__=1" "__LP64__" \
    "$spu" "${spu_extra[@]}"
# The last data-model option wins, in either order.
check "PPU -mlp64 -mno-lp64" "$common __LP32__=1 _LP32=1 __POINTER_32BIT__=1" "__LP64__ _LP64"     "$ppu" "${ppu_extra[@]}" -mlp64 -mno-lp64
check "PPU -mno-lp64 -mlp64" "$common __LP64__=1 _LP64=1" "__LP32__ _LP32 __POINTER_32BIT__"     "$ppu" "${ppu_extra[@]}" -mno-lp64 -mlp64
# A user -U still wins over the predefine.
check "PPU -U__CELLOS_LV2__" "__STRICT_ALIGNED=1" "__CELLOS_LV2__" \
    "$ppu" "${ppu_extra[@]}" -U__CELLOS_LV2__
check "SPU -U__CELLOS_LV2__" "__STRICT_ALIGNED=1 __FLOAT_FAST__=1" "__CELLOS_LV2__"     "$spu" "${spu_extra[@]}" -U__CELLOS_LV2__

[ "$status" -eq 0 ] && echo "platform-predefines: PASS"
exit $status

#!/usr/bin/env bash
# GCC patch 0066: -mlp64 has no meaning without the 64-bit mode.  The final
# options decide, not the command line:
#   -mlp64 -m32 and -m32 -mlp64           refused (exit 1, the diagnostic names -mlp64)
#   -mlp64 -m32 -m64 (and other orders that end 64-bit), -mlp64, default ILP32,
#   -m64, plain -m32                       compile, with the expected data model:
#     sizeof(void *) + sizeof(long), __LP64__ and _ARCH_PPC64 as listed below.
# C and C++ drivers.  Control: a valid state must compile here, so a compiler
# that refuses everything (or a missing driver) cannot pass the refusal rows.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: lp64-requires-64bit-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "lp64-requires-64bit: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"; cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "lp64-requires-64bit: FAIL: no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
extra=${EXTRA_CFLAGS:-}
printf 'int f(int *p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }\n' > "$work/t.c"
cp "$work/t.c" "$work/t.cc"
status=0

# flags | expected: "refuse" or "<ptr+long> <__LP64__> <_ARCH_PPC64>"
cases=(
    "|8 0 1"
    "-mlp64|16 1 1"
    "-m64|8 0 1"
    "-mlp64 -m64|16 1 1"
    "-m32|8 0 0"
    "-mlp64 -m32|refuse"
    "-m32 -mlp64|refuse"
    "-mlp64 -m32 -m64|16 1 1"
    "-m32 -mlp64 -m64|16 1 1"
    "-m32 -m64 -mlp64|16 1 1"
)
for lang in c c++; do
    drv=$cc; src="$work/t.c"; [ "$lang" = c++ ] && { drv=$cxx; src="$work/t.cc"; }
    for c in "${cases[@]}"; do
        flags=${c%%|*}; want=${c#*|}
        label="$lang [${flags:-default}]"
        # shellcheck disable=SC2086
        "$drv" $extra $flags -O2 -c "$src" -o "$work/t.o" > "$work/err" 2>&1; rc=$?
        if [ "$want" = refuse ]; then
            if [ $rc -eq 1 ] && grep -q "'-mlp64' requires the 64-bit mode" "$work/err"; then
                echo "lp64-requires-64bit: ok   refused $label"
            else
                echo "lp64-requires-64bit: FAIL $label: want exit 1 naming -mlp64, got exit $rc"
                sed 's/^/    /' "$work/err" | head -3; status=1
            fi
            continue
        fi
        if [ $rc -ne 0 ]; then
            echo "lp64-requires-64bit: FAIL $label: compile failed (exit $rc)"; sed 's/^/    /' "$work/err" | head -3; status=1; continue
        fi
        # shellcheck disable=SC2086
        printf '#if defined(__LP64__)\nint lp64 = 1;\n#else\nint lp64 = 0;\n#endif\n#if defined(_ARCH_PPC64)\nint a64 = 1;\n#else\nint a64 = 0;\n#endif\nint sz = sizeof(void *) + sizeof(long);\n' |
            "$drv" $extra $flags -x "$lang" -O0 -fno-zero-initialized-in-bss -S - -o "$work/m.s" 2>/dev/null
        # a zero-valued int is emitted as ".zero 4", a non-zero one as ".long N"
        got=$(awk '/^lp64:|^a64:|^sz:/{n=$1} /\.long|\.zero/{if(n){v[n]=($1==".zero")?0:$2; n=""}} END{print v["sz:"], v["lp64:"], v["a64:"]}' "$work/m.s")
        if [ "$got" = "$want" ]; then
            echo "lp64-requires-64bit: ok   $label -> $got"
        else
            echo "lp64-requires-64bit: FAIL $label: want '$want' (ptr+long LP64 ARCH_PPC64), got '$got'"; status=1
        fi
    done
done
[ $status -eq 0 ] && echo "PASS: lp64-requires-64bit-test"
exit $status

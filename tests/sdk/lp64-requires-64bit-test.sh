#!/usr/bin/env bash
# GCC patch 0066: -mlp64 has no meaning without the 64-bit mode.  The final
# options decide, not the command line:
#   -mlp64 -m32 and -m32 -mlp64           refused (exit 1, the diagnostic names -mlp64)
#   -mlp64 -m32 -m64 (and other orders that end 64-bit), -mlp64, default ILP32,
#   -m64, plain -m32                       compile, with the expected data model:
#     sizeof(void *) + sizeof(long), __LP64__ and _ARCH_PPC64 as listed below.
# C and C++ drivers.  Every row starts from removed outputs: a compile must exit
# 0 AND write a fresh, non-empty object (the -c row) and assembly (the probe
# row), or the row fails with the compiler's stderr.
# Self-check: the same rows run through wrapper drivers that fail only the
# probe compile of one valid row (-m32 -mlp64 -m64); the harness must report
# exactly that row (C and C++) as failed, so a stale probe listing can never
# pass a row.
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
printf '#if defined(__LP64__)\nint lp64 = 1;\n#else\nint lp64 = 0;\n#endif\n#if defined(_ARCH_PPC64)\nint a64 = 1;\n#else\nint a64 = 0;\n#endif\nint sz = sizeof(void *) + sizeof(long);\n' > "$work/probe.src"

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

# check_rows <tag> <c driver> <c++ driver>: one line per row; returns 0 only if all pass.
check_rows() {
    local tag=$1 ccd=$2 cxxd=$3 st=0 lang drv src c flags want label rc got
    for lang in c c++; do
        drv=$ccd; src="$work/t.c"; [ "$lang" = c++ ] && { drv=$cxxd; src="$work/t.cc"; }
        for c in "${cases[@]}"; do
            flags=${c%%|*}; want=${c#*|}
            label="$lang [${flags:-default}]"
            rm -f "$work/t.o" "$work/m.s" "$work/err" "$work/perr"
            # shellcheck disable=SC2086
            "$drv" $extra $flags -O2 -c "$src" -o "$work/t.o" > "$work/err" 2>&1; rc=$?
            if [ "$want" = refuse ]; then
                if [ $rc -eq 1 ] && grep -q "'-mlp64' requires the 64-bit mode" "$work/err"; then
                    echo "$tag: ok   refused $label"
                else
                    echo "$tag: FAIL $label: want exit 1 naming -mlp64, got exit $rc"
                    sed 's/^/    /' "$work/err" | head -3; st=1
                fi
                continue
            fi
            if [ $rc -ne 0 ] || [ ! -s "$work/t.o" ]; then
                echo "$tag: FAIL $label: compile exit $rc, object $( [ -s "$work/t.o" ] && echo written || echo missing)"
                sed 's/^/    /' "$work/err" | head -3; st=1; continue
            fi
            # shellcheck disable=SC2086
            "$drv" $extra $flags -x "$lang" -O0 -fno-zero-initialized-in-bss -S "$work/probe.src" -o "$work/m.s" 2> "$work/perr"; rc=$?
            if [ $rc -ne 0 ] || [ ! -s "$work/m.s" ]; then
                echo "$tag: FAIL $label: data-model probe exit $rc, listing $( [ -s "$work/m.s" ] && echo written || echo missing)"
                sed 's/^/    /' "$work/perr" | head -3; st=1; continue
            fi
            # a zero-valued int is emitted as ".zero 4", a non-zero one as ".long N"
            got=$(awk '/^lp64:|^a64:|^sz:/{n=$1} /[.]long|[.]zero/{if(n){v[n]=($1==".zero")?0:$2; n=""}} END{print v["sz:"], v["lp64:"], v["a64:"]}' "$work/m.s")
            if [ "$got" = "$want" ]; then
                echo "$tag: ok   $label -> $got"
            else
                echo "$tag: FAIL $label: want '$want' (ptr+long LP64 ARCH_PPC64), got '$got'"; st=1
            fi
        done
    done
    return $st
}

status=0
check_rows lp64-requires-64bit "$cc" "$cxx" > "$work/main.txt" || status=1
cat "$work/main.txt"

# Self-check: wrapper drivers that fail ONLY the probe compile of -m32 -mlp64 -m64.
for d in gcc g++; do
    real=$cc; [ "$d" = g++ ] && real=$cxx
    {
        echo '#!/usr/bin/env bash'
        echo 'case " $* " in'
        echo '    *" -m32 -mlp64 -m64 "*"-fno-zero-initialized-in-bss"*) echo "planted probe failure" >&2; exit 1 ;;'
        echo 'esac'
        echo "exec \"$real\" \"\$@\""
    } > "$work/bad-$d"
    chmod +x "$work/bad-$d"
done
check_rows self-check "$work/bad-gcc" "$work/bad-g++" > "$work/self.txt"; selfrc=$?
# Compare against the main run: both planted probe rows (c and c++) must fail,
# and no other row may change verdict (none newly failed, none lost).
nrow=$(grep -c 'FAIL .*\[-m32 -mlp64 -m64\]: data-model probe exit 1, listing missing' "$work/self.txt")
grep -o 'FAIL [^:]*' "$work/main.txt" | sort > "$work/main.fail"
grep -o 'FAIL [^:]*' "$work/self.txt" | sort > "$work/self.fail"
nadded=$(comm -13 "$work/main.fail" "$work/self.fail" | grep -vc -- '-m32 -mlp64 -m64')
nlost=$(comm -23 "$work/main.fail" "$work/self.fail" | wc -l)
if [ $selfrc -ne 0 ] && [ "$nrow" -eq 2 ] && [ "$nadded" -eq 0 ] && [ "$nlost" -eq 0 ]; then
    echo "lp64-requires-64bit: ok   self-check: a failing probe compile fails exactly its row (c and c++)"
else
    echo "lp64-requires-64bit: FAIL self-check: planted probe failure not caught as exactly 2 rows (rc $selfrc, probe rows $nrow, other rows newly failed $nadded, lost $nlost)"
    grep -v ': ok ' "$work/self.txt" | sed 's/^/    /' | head -8; status=1
fi

[ $status -eq 0 ] && echo "PASS: lp64-requires-64bit-test"
exit $status

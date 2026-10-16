#!/usr/bin/env bash
# -mminimal-toc compiles in both PPU ABIs (GCC patch 0043).  Under ILP32 every
# function that touched the TOC died with "unrecognizable insn" (the
# load_toc_aix_si pattern required TARGET_32BIT); the reference emits a
# 4-byte TOC slot for the minimal TOC's base and loads it with lwz.
#
# Rows: C and C++ at -O0/-O2 in both ABIs compile; in ILP32 the base slot is
# one 4-byte word (R_PPC64_ADDR32) loaded with lwz from r2, in LP64 it stays
# an 8-byte slot loaded with ld.
# usage: minimal-toc-ilp32-test.sh [--ps3dev DIR] [-B DIR]
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
    echo "minimal-toc: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
bin="$ps3dev/ppu/bin/powerpc64-ps3-elf"
for t in gcc g++ objdump; do
    [ -x "$bin-$t" ] || { echo "minimal-toc: FAIL: no $bin-$t" >&2; exit 1; }
done
B=(); [ -n "$bdir" ] && B=("-B$bdir")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "minimal-toc: FAIL: $*" >&2; status=1; }
ok() { echo "minimal-toc: ok   $*"; }

cat > "$work/t.c" <<'EOF'
int g;
static int table[4] = {1, 2, 3, 4};
extern int ext(int);
int (*hook)(int) = ext;
int f(int i) { return g + table[i & 3] + hook(i); }
EOF
cat > "$work/t.cpp" <<'EOF'
#include <string>
static std::string name = "toc";
int g2;
int h() { return (int)name.size() + g2; }
EOF

for abi in ilp32 lp64; do
    flag=(); [ "$abi" = lp64 ] && flag=(-mlp64)
    for opt in -O0 -O2; do
        for lang in c cpp; do
            drv="$bin-gcc"; [ "$lang" = cpp ] && drv="$bin-g++"
            obj="$work/$abi$opt.$lang.o"
            if "$drv" "${B[@]}" "${flag[@]}" "$opt" -mminimal-toc -c "$work/t.$lang" -o "$obj" 2>"$work/err"; then
                ok "$abi $opt $lang compiles"
            else
                fail "$abi $opt $lang: $(grep -m1 -E 'error|internal' "$work/err")"
            fi
        done
    done
    obj="$work/$abi-O2.c.o"
    [ -f "$obj" ] || continue
    # Each dump must itself succeed: a failed objdump prints nothing, and
    # nothing must not read as "no match" inside a pipeline.
    if ! dis=$("$bin-objdump" -dr "$obj" 2>"$work/objdump.err"); then
        fail "$abi objdump -dr failed: $(head -1 "$work/objdump.err")"; continue
    fi
    if ! toc=$("$bin-objdump" -r -j .toc "$obj" 2>"$work/objdump.err"); then
        fail "$abi objdump -r -j .toc failed: $(head -1 "$work/objdump.err")"; continue
    fi
    if [ "$abi" = ilp32 ]; then
        printf '%s\n' "$dis" | grep -Eq 'lwz +r?30,0\(r?2\)' \
            && ok "ilp32 loads the minimal TOC base with lwz from r2" \
            || fail "ilp32 base load is not lwz 30,0(2)"
        printf '%s\n' "$dis" | grep -A1 -E 'lwz +r?30,0\(r?2\)' | grep -q 'R_PPC64_TOC16' \
            && ok "ilp32 base load is TOC-relative" || fail "ilp32 base load lacks a TOC16 relocation"
        printf '%s\n' "$toc" | grep -q 'R_PPC64_ADDR32' \
            && ok "ilp32 base slot is a 4-byte ADDR32 word" || fail "ilp32 base slot is not ADDR32"
    else
        printf '%s\n' "$dis" | grep -Eq 'ld +r?30,0\(r?2\)' \
            && ok "lp64 loads the minimal TOC base with ld from r2" \
            || fail "lp64 base load is not ld 30,0(2)"
        printf '%s\n' "$toc" | grep -q 'R_PPC64_ADDR64' \
            && ok "lp64 base slot is an 8-byte ADDR64 word" || fail "lp64 base slot is not ADDR64"
    fi
done

# A 64-bit integer constant under a TOC that holds constants (-mminimal-toc,
# -mcmodel=small).  Since 0059 (ILP32 Pmode is DImode) a DImode constant was
# taken for a TOC entry, but an ILP32 TOC slot is 4 bytes, and -O0 died with
# an internal compiler error in output_toc.  Every cell compiles, and the
# whole value is still there: one 8-byte constant (.quad, hex or decimal),
# or built inline in ONE register in order (lis 0x1234, ori 0x5678, sldi 32,
# oris 0x9abc, ori 0xdef0).  A 4-byte .long of the value, or an inline
# sequence missing a step, does not count (negative controls below).  A
# Windows-hosted compiler writes CRLF assembly; the check strips CR.
whole_value() {
    grep -Eq '^[[:space:]]*\.quad[[:space:]]+(0x123456789abcdef0|1311768467463790320)[[:space:]]*$' "$1" && return 0
    tr -d '\r' < "$1" | tr '\n' ';' | grep -Eq 'lis ([0-9]+),0x1234;([^;]*;)*[[:space:]]*ori \1,\1,0x5678;([^;]*;)*[[:space:]]*sldi \1,\1,32;([^;]*;)*[[:space:]]*oris \1,\1,0x9abc;([^;]*;)*[[:space:]]*ori \1,\1,0xdef0;' && return 0
    split_halves "$1"
}
# split_halves <file>: GCC 16 builds the two words in two registers and joins them: rldimi A,B,32,0 inserts B's
# low word as A's high word, keeping A's low word.  A must be lis 0x9abc + ori 0xdef0 (the low word; the sign
# extension of lis lands in the high word, which rldimi replaces), B lis 0x1234 + ori 0x5678 (the high word).
split_halves() {
    local text a b
    text=$(tr -d '\r' < "$1")
    while read -r a b; do
        grep -Eq "^[[:space:]]*lis $a,0x9abc$" <<< "$text" && grep -Eq "^[[:space:]]*ori $a,$a,0xdef0$" <<< "$text" &&
            grep -Eq "^[[:space:]]*lis $b,0x1234$" <<< "$text" && grep -Eq "^[[:space:]]*ori $b,$b,0x5678$" <<< "$text" &&
            return 0
    done < <(sed -nE 's/^[[:space:]]*rldimi ([0-9]+),([0-9]+),32,0$/\1 \2/p' <<< "$text")
    return 1
}
printf '\t.long\t0x123456789abcdef0\n' > "$work/neg-long.s"
printf '\tlis 3,0x1234\n\tori 3,3,0x5678\n\toris 3,3,0x9abc\n\tori 3,3,0xdef0\n' > "$work/neg-nosldi.s"
printf '\tlis 3,0x1234\n\tori 4,4,0x5678\n\tsldi 3,3,32\n\toris 3,3,0x9abc\n\tori 3,3,0xdef0\n' > "$work/neg-tworegs.s"
printf '\tlis 3,0x1234\r\n\tori 3,3,0x5678\r\n\tsldi 3,3,32\r\n\toris 3,3,0x9abc\r\n\tori 3,3,0xdef0\r\n' > "$work/pos-inline.s"
printf '.LC0:\n\t.quad\t1311768467463790320\n' > "$work/pos-quad.s"
printf '\tlis 3,0x9abc\n\tlis 9,0x1234\n\tori 3,3,0xdef0\n\tori 9,9,0x5678\n\trldimi 3,9,32,0\n' > "$work/pos-split.s"
printf '\tlis 3,0x9abc\n\tlis 9,0x1234\n\tori 3,3,0xdef0\n\tori 9,9,0x5678\n\trldimi 3,9,16,0\n' > "$work/neg-split-shift.s"
printf '\tlis 3,0x1234\n\tlis 9,0x9abc\n\tori 3,3,0x5678\n\tori 9,9,0xdef0\n\trldimi 3,9,32,0\n' > "$work/neg-split-swapped.s"
if whole_value "$work/pos-inline.s" && whole_value "$work/pos-quad.s" && ! whole_value "$work/neg-long.s" &&
   ! whole_value "$work/neg-nosldi.s" && ! whole_value "$work/neg-tworegs.s" &&
   whole_value "$work/pos-split.s" && ! whole_value "$work/neg-split-shift.s" && ! whole_value "$work/neg-split-swapped.s"; then
    ok "64-bit value checker: inline, split-halves (rldimi) and .quad accepted; .long, a missing sldi, a register change, a wrong rldimi shift and swapped halves refused"
else
    fail "64-bit value checker does not separate its controls"
fi
cat > "$work/k.c" <<'EOF'
unsigned long long k1(void) { return 0x123456789abcdef0ULL; }
unsigned long long k2(unsigned long long x) { return x ^ 0x0fedcba987654321ULL; }
EOF
for abi in ilp32 lp64; do
    flag=(); [ "$abi" = lp64 ] && flag=(-mlp64)
    for model in -mminimal-toc -mcmodel=small; do
        for opt in -O0 -O2 -Os; do
            s="$work/k$abi$model$opt.s"
            if ! "$bin-gcc" "${B[@]}" "${flag[@]}" "$model" "$opt" -S "$work/k.c" -o "$s" 2>"$work/err"; then
                fail "64-bit constant, $abi $model $opt: $(grep -m1 -E 'error|internal' "$work/err")"; continue
            fi
            if whole_value "$s"; then
                ok "64-bit constant, $abi $model $opt compiles with its whole value"
            else
                fail "64-bit constant, $abi $model $opt: 0x123456789abcdef0 is not in the assembly"
            fi
        done
    done
done

[ "$status" -eq 0 ] && echo "minimal-toc: PASS"
exit $status

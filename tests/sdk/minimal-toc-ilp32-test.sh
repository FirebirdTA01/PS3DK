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

[ "$status" -eq 0 ] && echo "minimal-toc: PASS"
exit $status

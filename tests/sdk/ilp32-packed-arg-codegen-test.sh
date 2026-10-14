#!/usr/bin/env bash
# Codegen guard: a pointer taken out of a by-value aggregate that arrives
# packed in one 64-bit GPR (std::string_view, {pointer, length} pairs) is
# never used as an address with the other member still in bits 32-63.  GCC
# patch 0033 canonicalised the extracted pointer while Pmode was SImode; the
# DImode Pmode of 0059 covers it now (0033 retired).  The 2026-10-09 Mossbrook
# freeze (measureText(std::string_view)) was this class.
#
# Compiles the probes below (C aggregates with the pointer in either half; a
# C++ string_view loop shaped like the Mossbrook function) in both data
# models at -O0/-O1/-O2/-O3/-Os and runs scripts/ppu-packed-arg-scan.py on
# every object: it must find nothing.  Each row removes its outputs first and
# requires the compile to exit 0 with a non-empty object.
# Red control: the scanner's red_copy fixture (split r4, load through a copy
# of the raw register), assembled here, must be reported (exit 1), so a dead
# scanner cannot pass the rows.  The scan is linear and only sees functions
# that split the high member with a 32-bit shift: a clean result is not a proof.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: ilp32-packed-arg-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "ilp32-packed-arg: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
bin="$ps3dev/ppu/bin"
cc="$bin/powerpc64-ps3-elf-gcc"; cxx="$bin/powerpc64-ps3-elf-g++"
as="$bin/powerpc64-ps3-elf-as"; od="$bin/powerpc64-ps3-elf-objdump"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ilp32-packed-arg: FAIL: no PPU compiler under $ps3dev"; exit 1; }
py=$(command -v python3 || command -v python) || { echo "ilp32-packed-arg: FAIL: no python"; exit 1; }
scan="$root/scripts/ppu-packed-arg-scan.py"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
extra=${EXTRA_CFLAGS:-}
status=0

# Red control: the scanner must report a function that loads through a copy
# of the raw packed register after splitting it.
{
    printf '\t.section .text.red_copy,"ax",@progbits\n\t.globl red_copy\nred_copy:\n'
    printf '\tsradi 9,4,32\n\tmr 10,4\n\tlbz 3,0(10)\n\tadd 3,3,9\n\tblr\n'
} > "$work/red.s"
rm -f "$work/red.o"
if "$as" -a64 "$work/red.s" -o "$work/red.o" 2> "$work/as.err" && [ -s "$work/red.o" ]; then
    "$py" "$scan" --objdump "$od" "$work/red.o" > "$work/red.out" 2>&1; rc=$?
    if [ $rc -eq 1 ]; then echo "ilp32-packed-arg: ok   red control: the scanner reports red_copy"
    else echo "ilp32-packed-arg: FAIL red control: scanner exit $rc on red_copy"; sed 's/^/    /' "$work/red.out" | head -4; status=1; fi
else
    echo "ilp32-packed-arg: FAIL red control: could not assemble"; sed 's/^/    /' "$work/as.err" | head -3; status=1
fi

cat > "$work/agg.c" <<'EOF'
struct pl { const char *p; unsigned n; };
struct lp { unsigned n; const char *p; };
struct pp { const char *a; const char *b; };
extern void g (void);
int sum_pl (struct pl s) { int t = 0; for (unsigned i = 0; i < s.n; i++) t += s.p[i]; return t; }
int sum_lp (struct lp s) { int t = 0; for (unsigned i = 0; i < s.n; i++) t += s.p[i]; return t; }
int first_pl (struct pl s) { g (); return s.p[0] + s.n; }
int first_lp (struct lp s) { g (); return s.p[0] + s.n; }
int both (struct pp s) { return s.a[1] - s.b[2]; }
void copy_pl (struct pl s, char *d) { for (unsigned i = 0; i < s.n; i++) d[i] = s.p[i]; }
EOF
cat > "$work/sv.cc" <<'EOF'
#include <string_view>
#include <map>
struct Glyph { float adv; };
struct Builder {
    std::map<unsigned char, Glyph> glyphs;
    float measureText (std::string_view s) const;
    unsigned count (std::string_view s, char c) const;
};
float Builder::measureText (std::string_view s) const
{
    float w = 0;
    for (unsigned char c : s) { auto it = glyphs.find (c); w += it != glyphs.end () ? it->second.adv : 1.0f; }
    return w;
}
unsigned Builder::count (std::string_view s, char c) const
{
    unsigned n = 0;
    for (char x : s) n += x == c;
    return n;
}
EOF
for f in agg.c sv.cc; do
    drv=$cc; std=; [ "$f" = sv.cc ] && { drv=$cxx; std=-std=gnu++17; }
    for abi in "" -mlp64; do for opt in -O0 -O1 -O2 -O3 -Os; do
        label="$f ${abi:-ilp32} $opt"
        rm -f "$work/o.o" "$work/err"
        # shellcheck disable=SC2086
        "$drv" $extra $abi $opt $std -c "$work/$f" -o "$work/o.o" 2> "$work/err"; rc=$?
        if [ $rc -ne 0 ] || [ ! -s "$work/o.o" ]; then
            echo "ilp32-packed-arg: FAIL $label: compile exit $rc"; sed 's/^/    /' "$work/err" | head -3; status=1; continue
        fi
        "$py" "$scan" --objdump "$od" "$work/o.o" > "$work/scan.out" 2>&1; rc=$?
        case $rc in
            0) echo "ilp32-packed-arg: ok   $label" ;;
            1) echo "ilp32-packed-arg: FAIL $label: packed-register address"; sed 's/^/    /' "$work/scan.out" | head -6; status=1 ;;
            *) echo "ilp32-packed-arg: FAIL $label: scanner exit $rc"; sed 's/^/    /' "$work/scan.out" | head -3; status=1 ;;
        esac
    done; done
done
[ $status -eq 0 ] && echo "PASS: ilp32-packed-arg-codegen-test"
exit $status

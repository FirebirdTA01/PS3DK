#!/usr/bin/env bash
# Codegen guard for GCC patch 0084: on the Cell PPU, BRANCH_COST is set
# to 8 (was 3 in rs6000.h).
#
# The Cell PPU is in-order with a long pipeline: a mispredicted
# conditional branch costs more than on the out-of-order POWER cores
# that the default BRANCH_COST=3 was tuned for.  BRANCH_COST is the
# threshold the middle end (ifcvt.cc, dojump.cc, regmodel.cc) uses to
# decide whether a branch-free substitute is worth emitting over a taken
# branch.  Raising it from 3 to 8 shifts the balance toward branch-free
# code when a branch-free form is reachable.
#
# For integer comparisons the result is already branch-free on Cell
# without 0084 -- the cstore<mode>4 expanders (rs6000.md:12417) dispatch
# to cstore<mode>4_unsigned (subf/carry) or cstore<mode>4_signed
# (lshr/subf/xor) before the mfcr fallback -- so these rows are a
# regression guard: they assert the branch-free end state holds.
#
#   cstore row: int / unsigned comparison results stored in a GPR must
#     not use the mfcr set<mode>_cc fallback (rs6000.md:12782).  No
#     `mfcr r,crN` in any of the cmp_* function bodies.
#
#   min/max row: imin / imax / vmin / vmax must not emit a conditional
#     branch (b<b<...) in their function bodies.
#
#   control row: a plain `if (x) f() else g()` (not a comparison-store
#     or min/max idiom) must still compile to a conditional branch.
#
# Observable on -S output in both data models at -O -O2 -Os.
#
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-branch-cost-8-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-branch-cost-8: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-branch-cost-8: FAIL: no PPU C compiler under $ps3dev"; exit 1; }
{ [ -x "$cxx" ] || [ -x "$cxx.exe" ]; } || { echo "ppu-branch-cost-8: FAIL: no PPU C++ compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

cat > "$work/cstore.c" <<'EOF'
int cmp_ge  (int a, int b)   { return a >= b; }
int cmp_gt  (int a, int b)   { return a >  b; }
int cmp_le  (int a, int b)   { return a <= b; }
int cmp_lt  (int a, int b)   { return a <  b; }
unsigned cmp_geu (unsigned a, unsigned b) { return a >= b; }
unsigned cmp_gtu (unsigned a, unsigned b) { return a >  b; }
EOF

cat > "$work/minmax.cpp" <<'EOF'
int imin (int a, int b)   { return a < b ? a : b; }
int imax (int a, int b)   { return a > b ? a : b; }
unsigned vmin (unsigned a, unsigned b) { return a < b ? a : b; }
unsigned vmax (unsigned a, unsigned b) { return a > b ? a : b; }
EOF

cat > "$work/plain-if.c" <<'EOF'
extern void side_effect_a (void);
extern void side_effect_b (void);
int which = 0;
void pick (int x) { if (x) side_effect_a (); else side_effect_b (); }
EOF

# Helper: extract a function body from an -S output and emit it.
# GCC's .s layout for a function:
#   <fn>:
#   ...
#   blr
#   .size <fn>, .-<fn>
#   (optionally a second section, but we take the block up to .size)
fn_body() {  # $1 = file, $2 = function name
    awk -v n="$2" '
        $0 ~ "^" n ":" { f=1; next }
        f && /^[[:space:]]*\.size[[:space:]]/ { f=0; next }
        f { print }
    ' "$1"
}

fail=0

# self-check: the function-body extractor works on a synthetic TU.
synthetic() {
    local f=${1:-body}
    printf '%s:\n\taddi\t3,1,4\n\tblr\n\t.size %s, .-%s\n' "$f" "$f" "$f"
}
synthetic synth_fn > "$work/s.s"
got="$(fn_body "$work/s.s" synth_fn)"
want="$(printf '\taddi\t3,1,4\n\tblr')"
if [ "$got" = "$want" ]; then
    echo "ppu-branch-cost-8: ok   self-check (function body extractor)"
else
    echo "ppu-branch-cost-8: FAIL self-check (function body extractor returned: $(fn_body "$work/s.s" synth_fn | head -2 | tr '\n' ' '))"
    fail=1
fi

for abi in "" -mlp64; do
    for opt in -O -O2 -Os; do
        label="${abi:-ilp32} $opt"

        # cstore: no mfcr reference in the function body.
        if out=$("$cc" $abi $opt -S "$work/cstore.c" -o "$work/c.s" 2>&1); then
            mfcrs=0
            for fn in cmp_ge cmp_gt cmp_le cmp_lt cmp_geu cmp_gtu; do
                body=$(fn_body "$work/c.s" "$fn")
                n=$(printf '%s\n' "$body" | grep -cE '^[[:space:]]*mfcr[[:space:]]+r?[0-9]+,[[:space:]]*cr[0-7]' || true)
                if [ "$n" -ne 0 ]; then
                    mfcrs=$((mfcrs + n))
                    echo "ppu-branch-cost-8: FAIL $label $fn $n mfcr/cr lines:"
                    printf '%s\n' "$body" | grep -E 'mfcr' | head -3 | sed 's/^/    /'
                fi
            done
            if [ "$mfcrs" -eq 0 ]; then
                echo "ppu-branch-cost-8: ok   $label cstore (no mfcr/cr reference)"
            else
                fail=1
            fi
        else
            echo "ppu-branch-cost-8: FAIL $label cstore compile"; printf '%s\n' "$out" | head -5
            fail=1; continue
        fi

        # min/max: no conditional branch in the function body.
        if out=$("$cxx" $abi $opt -std=c++17 -S "$work/minmax.cpp" -o "$work/m.s" 2>&1); then
            branched=0
            for fn in imin imax vmin vmax; do
                body=$(fn_body "$work/m.s" "$fn")
                n=$(printf '%s\n' "$body" | grep -cE '^[[:space:]]*(beq|bne|bgt|bge|blt|ble)[+ld-]*([[:space:]]|$)' || true)
                if [ "$n" -ne 0 ]; then
                    branched=$((branched + n))
                    echo "ppu-branch-cost-8: FAIL $label $fn $n conditional branch(es):"
                    printf '%s\n' "$body" | grep -E '^[[:space:]]*(beq|bne|bgt|bge|blt|ble)[+ld-]*([[:space:]]|$)' | head -3 | sed 's/^/    /'
                fi
            done
            if [ "$branched" -eq 0 ]; then
                echo "ppu-branch-cost-8: ok   $label minmax (no branches in imin/imax/vmin/vmax)"
            else
                fail=1
            fi
        else
            echo "ppu-branch-cost-8: FAIL $label minmax compile"; printf '%s\n' "$out" | head -5
            fail=1; continue
        fi

        # control: a plain `if` branch (not a comparison-store, min/max,
        # or loop back) is not subject to BRANCH_COST and still compiles
        # to a conditional branch.  This proves that BRANCH_COST=8 does
        # not accidentally strip every `bne`/`beq` from the TU.
        if out=$("$cc" $abi $opt -S "$work/plain-if.c" -o "$work/p.s" 2>&1); then
            body=$(fn_body "$work/p.s" pick)
            if printf '%s\n' "$body" | grep -qE '^[[:space:]]*(beq|bne|bgt|bge|blt|ble)[+ld-]*([[:space:]]|$)'; then
                echo "ppu-branch-cost-8: ok   $label control (plain if still emits a conditional branch)"
            else
                echo "ppu-branch-cost-8: FAIL $label control: plain if lost its conditional branch"
                printf '%s\n' "$body" | head -10 | sed 's/^/    /'
                fail=1
            fi
        else
            echo "ppu-branch-cost-8: FAIL $label control compile"; printf '%s\n' "$out" | head -5
            fail=1; continue
        fi
    done
done

[ $fail -eq 0 ] && echo "ppu-branch-cost-8: PASS"
exit $fail

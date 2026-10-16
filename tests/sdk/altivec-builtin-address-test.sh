#!/usr/bin/env bash
# AltiVec memory builtins whose operand is a pointer must compile under the
# ILP32 address model (GCC patch 0059).  With Pmode DImode and 32-bit
# pointers, the builtin expanders forced a ptr_mode (SImode) pointer into a
# Pmode register and the compiler aborted ("internal compiler error: in
# copy_to_mode_reg"); the vectormath headers (floatInVec, vec_lvsl) hit it.
#
# Compiles every AltiVec load/store/permute-control builtin that takes a
# pointer, from a parameter, a computed integer address and a global, at -O0
# and -O2 in both data models; under ILP32 the computed address reaching
# lvsl must be zero-extended (rldic/rldicl/clrldi with 32 high bits cleared),
# a computed byte offset sign-extended, and a 0x80000000 constant pointer
# zero-extended.
# The v0.20 candidate 23df4007 compiler is the red control: it aborts here;
# the 9a01fab2 builtin expander is the red control for the offset and
# constant-pointer rows.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: altivec-builtin-address-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "altivec-builtin-address: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "altivec-builtin-address: FAIL: no compiler at $cc"; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/vb.c" <<'EOF'
#include <altivec.h>
float g[64] __attribute__((aligned(16)));
vector unsigned char lvsl_param (float *p) { return vec_lvsl (0, p); }
vector unsigned char lvsl_computed (int slot) { return vec_lvsl (0, (float *) (unsigned long) (slot << 2)); }
vector unsigned char lvsr_param (float *p) { return vec_lvsr (4, p); }
vector float ld_param (float *p, int o) { return vec_ld (o, p); }
vector float ld_global (int o) { return vec_ld (o, g); }
vector float ldl_param (float *p) { return vec_ldl (16, p); }
vector float lde_param (float *p) { return vec_lde (8, p); }
void st_param (vector float v, float *p, int o) { vec_st (v, o, p); }
void stl_param (vector float v, float *p) { vec_stl (v, 32, p); }
void ste_param (vector float v, float *p) { vec_ste (v, 4, p); }
vector float ld_neg (float *p, int a, int b) { return vec_ld (a - b, p); }
void st_neg (vector float v, float *p, int a, int b) { vec_st (v, a - b, p); }
vector unsigned char lvsl_neg (float *p, int a, int b) { return vec_lvsl (a - b, p); }
vector unsigned char lvsr_neg (float *p, int a, int b) { return vec_lvsr (a - b, p); }
vector float lde_neg (float *p, int a, int b) { return vec_lde (a - b, p); }
void ste_neg (vector float v, float *p, int a, int b) { vec_ste (v, a - b, p); }
vector float ldl_neg (float *p, int a, int b) { return vec_ldl (a - b, p); }
void stl_neg (vector float v, float *p, int a, int b) { vec_stl (v, a - b, p); }
vector unsigned char lvsl_const (void) { return vec_lvsl (0, (float *) 0x80000004u); }
vector float lde_const (void) { return vec_lde (0, (float *) 0x80000004u); }
void ste_const (vector float v) { vec_ste (v, 0, (float *) 0x80000004u); }
EOF

# ea_is_const <body> <op>: run the function body in order (tests/lib/ppc-const-eval.sh) up to its first <op>
# instruction - the builtin's own access (lvsl, lvewx, stvewx); at -O0 a vector argument is first spilled with stvx,
# which is not it.  That access's effective address must be exactly 0x0000000080000004 - the 32-bit address,
# zero-extended, whatever instructions built it (li/oris, lis/ori/rldicl, ...).  An unknown or different value fails.
# shellcheck source=../lib/ppc-const-eval.sh
source "$(dirname "${BASH_SOURCE[0]}")/../lib/ppc-const-eval.sh"
ea_is_const() {
    ppc_eval "$1" "^$2[[:space:]]"
    [[ -n $PPC_STOP_LINE && $(ppc_ea "$PPC_STOP_LINE") == 0x0000000080000004 ]]
}

status=0
# The checker's own controls: GCC 16's li/oris build and GCC 12's lis/ori/clrldi build are accepted, also after an
# -O0 stvx spill; a sign-extending lis, a negative li, an access through another register, a register clobbered
# before the oris or before the access (codex's 7bf0e74f counterexamples), and a clobber after a spill are refused;
# a value spilled and reloaded is accepted, but not after a narrower store overwrote part of the slot, nor past a
# branch (codex's 902c3f6a counterexamples); an indexed update form changing the base (stdux, ldux - codex's
# 03acb1eb counterexamples) and any opcode the evaluator does not list are refused too.
if ea_is_const $'\tli 9,4\n\toris 9,9,0x8000\n\tlvsl 0,0,9' lvsl &&
   ea_is_const $'\tlis 9,0x8000\n\tori 9,9,4\n\tclrldi 9,9,32\n\tlvsl 0,0,9' lvsl &&
   ea_is_const $'li 9,48\naddi 10,31,64\nstvx 2,10,9\nli 9,4\noris 9,9,0x8000\nstvewx 0,0,9' stvewx &&
   ! ea_is_const $'\tlis 9,0x8000\n\tori 9,9,4\n\tlvsl 0,0,9' lvsl &&
   ! ea_is_const $'\tli 9,-4\n\toris 9,9,0x8000\n\tlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nlvsl 0,0,10' lvsl &&
   ! ea_is_const $'li 9,4\nli 9,-1\noris 9,9,0x8000\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nli 9,-1\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nstvx 2,0,9\nli 9,0\nstvewx 0,0,9' stvewx &&
   ea_is_const $'li 9,4\noris 9,9,0x8000\nstd 9,32(1)\nld 9,32(1)\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nstd 9,32(1)\nli 10,-1\nstw 10,32(1)\nld 9,32(1)\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'lis 9,0x8000\nori 9,9,4\nb .Laccess\nclrldi 9,9,32\n.Laccess:\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nli 10,0\nli 11,16\nstdux 10,9,11\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nli 10,0\nli 11,16\nldux 10,9,11\nlvsl 0,0,9' lvsl &&
   ! ea_is_const $'li 9,4\noris 9,9,0x8000\nfrobnicate 12,13\nlvsl 0,0,9' lvsl; then
    echo "altivec-builtin-address: ok   effective-address evaluator separates its controls"
else
    echo "altivec-builtin-address: FAIL effective-address evaluator does not separate its controls"; status=1
fi
for model in "" -mlp64; do
    for opt in -O0 -O2; do
        if "$cc" ${EXTRA_CFLAGS:-} -mcpu=cell -maltivec $model $opt -S "$work/vb.c" -o "$work/vb.s" 2> "$work/err.txt"; then
            echo "altivec-builtin-address: ok   ${model:-ILP32} $opt compiles"
        else
            echo "altivec-builtin-address: FAIL ${model:-ILP32} $opt"; grep -m2 -E "error|internal" "$work/err.txt"; status=1
            continue
        fi
        if [ -z "$model" ] && [ "$opt" = "-O2" ]; then
            body=$(sed -n '/^\.L\.lvsl_computed:/,/blr/p' "$work/vb.s")
            if echo "$body" | grep -qE 'rldic[lr]? [0-9]+,[0-9]+,[0-9]+,32|clrldi [0-9]+,[0-9]+,32'; then
                echo "altivec-builtin-address: ok   ILP32 -O2 computed lvsl address is zero-extended"
            else
                echo "altivec-builtin-address: FAIL ILP32 -O2 computed lvsl address not zero-extended"; echo "$body"; status=1
            fi
        fi
        [ -z "$model" ] || continue
        # A byte offset is a signed int: a computed (at -O0, spilled) one
        # sign-extends, never zero-extends as if it were a pointer.  lvx/stvx
        # mask the address in 32 bits, so the unmasked forms are the witnesses.
        for fn in lvsl_neg lvsr_neg lde_neg ste_neg ldl_neg stl_neg; do
            body=$(sed -n "/^\.L\.$fn:/,/blr/p" "$work/vb.s")
            if echo "$body" | grep -qE 'extsw ' && ! echo "$body" | grep -qE 'rldicl [0-9]+,[0-9]+,0,32|clrldi [0-9]+,[0-9]+,32'; then
                echo "altivec-builtin-address: ok   ILP32 $opt $fn offset is sign-extended"
            else
                echo "altivec-builtin-address: FAIL ILP32 $opt $fn offset not sign-extended"; echo "$body"; status=1
            fi
        done
        # A high-bit constant pointer is a 32-bit address: it must reach the
        # access as 0x80000004, not 0xffffffff80000004.  ea_is_const evaluates
        # the function body up to the access and checks the address itself.
        declare -A access=([lvsl_const]=lvsl [lde_const]=lvewx [ste_const]=stvewx)
        for fn in lvsl_const lde_const ste_const; do
            body=$(sed -n "/^\.L\.$fn:/,/blr/p" "$work/vb.s")
            if ea_is_const "$body" "${access[$fn]}"; then
                echo "altivec-builtin-address: ok   ILP32 $opt $fn constant pointer is zero-extended"
            else
                echo "altivec-builtin-address: FAIL ILP32 $opt $fn constant pointer not zero-extended"; echo "$body"; status=1
            fi
        done
    done
done

[ $status -eq 0 ] && echo "altivec-builtin-address: PASS" || echo "altivec-builtin-address: FAIL"
exit $status

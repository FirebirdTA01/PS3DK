#!/usr/bin/env bash
# ld's TOC edit must not carry heap bytes into the output (binutils patch 0007).
#
# --gc-sections drops TOC entries referenced only from removed sections, and
# ppc64_elf_edit_toc shuffles the rest down 8 bytes at a time.  CellOS Lv-2
# ILP32 entries are 4 bytes, so a .toc of an odd number of them is 4 mod 8
# long: unpatched, the last step read 4 bytes past ld's copy of the section
# and the rounded-up size wrote them into .got.  The same objects linked from
# output paths of different lengths then differed in that word.
#
# Generates a .toc of N 4-byte entries (N odd): entries 0 and 1 are referenced
# only from a section --gc-sections removes, the rest from the entry function.
# The first doubleword is dropped, so the edited .toc holds N-2 entries plus one
# pad word.  Each N is linked with glibc's MALLOC_PERTURB_ unset, 85 and 170 (a
# perturbed heap makes the over-read bytes nonzero, so the red control does not
# depend on heap luck), and the output .got must hold exactly: entry i = the
# address of d<i+2>, then a zero pad word.  The checker's own controls: a .got
# image with a nonzero or missing pad, a wrong entry, or no edit at all is refused.
#
# Linux hosts only (MALLOC_PERTURB_ is glibc's); skips without PS3DEV.
# usage: toc-edit-pad-test.sh [--ps3dev DIR]
set -u
ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "toc-edit-pad: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
if [ "$(uname -s)" != Linux ]; then
    echo "toc-edit-pad: SKIP (needs a glibc host for MALLOC_PERTURB_)"
    exit 0
fi
p="$ps3dev/ppu/bin/powerpc64-ps3-elf"
[ -x "$p-ld" ] && [ -x "$p-as" ] || { echo "toc-edit-pad: FAIL: no assembler/linker at $p-*"; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
status=0

# gen <n>: the test source.
gen() {
    local n=$1 i
    printf '\t.section .toc,"aw"\n\t.align 3\n'
    for ((i = 0; i < n; i++)); do printf '.LC%d:\t.long d%d\n' $i $i; done
    printf '\t.section .text.dead,"ax",@progbits\ndead:\n\tlwz 3,.LC0@toc(2)\n\tlwz 4,.LC1@toc(2)\n\tblr\n'
    printf '\t.section .text.live,"ax",@progbits\n\t.globl live\nlive:\n'
    for ((i = 2; i < n; i++)); do printf '\tlwz 3,.LC%d@toc(2)\n' $i; done
    printf '\tblr\n\t.section .data,"aw"\n'
    for ((i = 0; i < n; i++)); do printf 'd%d:\t.long 0x%08x\n' $i $((0x10000000 + i)); done
}

# got_ok <got hex words> <n> <d-symbol table "name addr" lines>: the .got must be
# exactly entries d2..d<n-1>, then one zero word.
got_ok() {
    local words=$1 n=$2 syms=$3 want="" i a
    for ((i = 2; i < n; i++)); do
        a=$(awk -v s="d$i" '$3 == s { print $1 }' <<< "$syms")
        [ -n "$a" ] || return 1
        want+=$(printf '%08x' $((16#$a & 0xffffffff)))
    done
    want+=00000000
    [ "$words" = "$want" ]
}

# The checker's own controls, on n=5 with d0..d4 at 0x08..0x18: the right image is accepted; a nonzero pad
# (either perturb pattern), a wrong entry, a missing pad and an unedited .toc are refused.
ctl_syms=$'0000000000000008 d d0\n000000000000000c d d1\n0000000000000010 d d2\n0000000000000014 d d3\n0000000000000018 d d4'
if got_ok 00000010000000140000001800000000 5 "$ctl_syms" &&
   ! got_ok 00000010000000140000001855555555 5 "$ctl_syms" &&
   ! got_ok 000000100000001400000018aaaaaaaa 5 "$ctl_syms" &&
   ! got_ok 00000010000000140000001900000000 5 "$ctl_syms" &&
   ! got_ok 000000100000001400000018 5 "$ctl_syms" &&
   ! got_ok 000000080000000c00000010000000140000001800000000 5 "$ctl_syms"; then
    echo "toc-edit-pad: ok   .got checker separates its controls"
else
    echo "toc-edit-pad: FAIL the .got checker does not separate its controls"; status=1
fi

for n in 9 33 69 129; do
    gen $n > "$work/t$n.s"
    if ! "$p-as" -a64 -mcell "$work/t$n.s" -o "$work/t$n.o" 2> "$work/err.txt"; then
        echo "toc-edit-pad: FAIL n=$n does not assemble"; head -3 "$work/err.txt"; status=1; continue
    fi
    for perturb in "" 85 170; do
        label="n=$n MALLOC_PERTURB_=${perturb:-unset}"
        if ! MALLOC_PERTURB_=$perturb "$p-ld" -e live --gc-sections "$work/t$n.o" -o "$work/t.elf" 2> "$work/err.txt"; then
            echo "toc-edit-pad: FAIL $label does not link"; head -3 "$work/err.txt"; status=1; continue
        fi
        words=$("$p-objcopy" -O binary --only-section=.got "$work/t.elf" /dev/stdout | od -An -v -tx1 | tr -d ' \n')
        syms=$("$p-nm" "$work/t.elf")
        if got_ok "$words" $n "$syms"; then
            echo "toc-edit-pad: ok   $label .got = $((n - 2)) kept entries + zero pad"
        else
            echo "toc-edit-pad: FAIL $label .got is $words"; status=1
        fi
    done
done

[ $status -eq 0 ] && echo "toc-edit-pad: PASS" || echo "toc-edit-pad: FAIL"
exit $status

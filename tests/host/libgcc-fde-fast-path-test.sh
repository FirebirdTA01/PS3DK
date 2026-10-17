#!/usr/bin/env bash
# The INSTALLED GCC 16 PPU libgcc looks FDEs up through the lock-free b-tree (gcc-16.x 0074), in both multilibs.
#
# Reads the unwind-dw2-fde object out of each multilib's libgcc archive and checks, by symbol and instruction:
#   - the b-tree registry is there (local registered_frames / registered_objects), the mutex-held list is not
#     (seen_objects / unseen_objects);
#   - the trees are kept to process end: no release_registered_frames destructor;
#   - the version-lock atomics have the pointer width: lwarx/stwcx. under ILP32, ldarx/stdcx. under -mlp64.
# A GCC older than 16 has no 0074 and is skipped.
#
# usage: libgcc-fde-fast-path-test.sh   (PS3DEV = the stage to check; exit 0 pass, 1 fail, 77 skipped)
set -u -o pipefail
: "${PS3DEV:?set PS3DEV to the stage}"
G=$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc
P=$PS3DEV/ppu/bin/powerpc64-ps3-elf
[ -x "$G" ] || { echo "libgcc-fde-fast-path: FAIL no compiler at $G"; exit 1; }
major=$("$G" -dumpversion | cut -d. -f1) || { echo "libgcc-fde-fast-path: FAIL -dumpversion"; exit 1; }
[ "$major" -ge 16 ] || { echo "libgcc-fde-fast-path: SKIP GCC $major has no 0074"; exit 77; }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "libgcc-fde-fast-path: FAIL $*"; status=1; abi_bad=1; }

for abi in ilp32 lp64; do
    abi_bad=0; flag=; [ $abi = lp64 ] && flag=-mlp64
    lib=$("$G" $flag -print-libgcc-file-name) || { fail "$abi: -print-libgcc-file-name"; continue; }
    [ -f "$lib" ] || { fail "$abi: no $lib"; continue; }
    member=$("$P-ar" t "$lib" | grep -x 'unwind-dw2-fde\.o') || { fail "$abi: no unwind-dw2-fde.o in $lib"; continue; }
    mkdir -p "$work/$abi"
    (cd "$work/$abi" && "$P-ar" x "$lib" "$member") || { fail "$abi: ar x"; continue; }
    o=$work/$abi/$member
    syms=$("$P-nm" "$o") || { fail "$abi: nm"; continue; }
    dis=$("$P-objdump" -d "$o") || { fail "$abi: objdump"; continue; }
    has() { awk '{print $NF}' <<<"$syms" | grep -qx "$1"; }
    for s in registered_frames registered_objects; do has $s || fail "$abi: b-tree symbol $s missing (list path still built?)"; done
    for s in seen_objects unseen_objects; do has $s && fail "$abi: list-path symbol $s present"; done
    has release_registered_frames && fail "$abi: release_registered_frames present (trees destroyed at exit)"
    if [ $abi = ilp32 ]; then want=lwarx; not=ldarx; else want=ldarx; not=lwarx; fi
    grep -qw "$want" <<<"$dis" || fail "$abi: no $want in the registry's atomics"
    n_not=$(grep -cw "$not" <<<"$dis" || true)
    [ $abi_bad -eq 0 ] && echo "libgcc-fde-fast-path: ok $abi $lib: b-tree registry, no list path, no tree destructor, $want present ($not x$n_not)"
done
[ $status -eq 0 ] && echo "PASS: libgcc-fde-fast-path-test"
exit $status

#!/usr/bin/env bash
# The INSTALLED GCC 16 PPU libgcc looks FDEs up through the lock-free b-tree (gcc-16.x 0074), in both multilibs.
#
# Reads the unwind-dw2-fde object out of each multilib's libgcc archive and checks, by symbol and instruction:
#   - the b-tree registry is there (local registered_frames / registered_objects), the mutex-held list is not
#     (seen_objects / unseen_objects);
#   - the trees are kept to process end: no release_registered_frames destructor;
#   - the version-lock atomics have the pointer width: lwarx/stwcx. under ILP32, ldarx/stdcx. under -mlp64.
# Every tool's exit status is checked, and a lookup tells "absent" from "could not be judged" (a negative check
# never passes on an error).  AR / NM / OBJDUMP / AWK may name other tools (the adversary test substitutes ones
# that print valid output and then fail - the whole test must fail).  A GCC older than 16 has no 0074 and skips.
#
# usage: libgcc-fde-fast-path-test.sh   (PS3DEV = the stage to check; skips without one, or on GCC < 16)
set -u -o pipefail
G=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc
P=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf
if [ -z "${PS3DEV:-}" ] || [ ! -x "$G" ]; then echo "SKIP: libgcc-fde-fast-path-test (no PS3DEV compiler)"; exit 0; fi
AR=${AR:-$P-ar}; NM=${NM:-$P-nm}; OBJDUMP=${OBJDUMP:-$P-objdump}; AWK=${AWK:-awk}
ver=$("$G" -dumpversion) || { echo "libgcc-fde-fast-path: FAIL -dumpversion"; exit 1; }
major=${ver%%.*}
[ "$major" -ge 16 ] || { echo "SKIP: libgcc-fde-fast-path-test (GCC $major has no 0074)"; exit 0; }

work=$(mktemp -d) || { echo "libgcc-fde-fast-path: FAIL mktemp"; exit 1; }
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "libgcc-fde-fast-path: FAIL $*"; status=1; abi_bad=1; }

# lookup <regex> <text>: 0 present, 1 absent, 2 could not be judged (grep error)
lookup() { grep -qx -- "$1" <<<"$2"; local rc=$?; [ $rc -le 1 ] && return $rc; return 2; }

for abi in ilp32 lp64; do
    abi_bad=0; flag=; [ $abi = lp64 ] && flag=-mlp64
    lib=$("$G" $flag -print-libgcc-file-name) || { fail "$abi: -print-libgcc-file-name"; continue; }
    [ -f "$lib" ] || { fail "$abi: no $lib"; continue; }
    members=$("$AR" t "$lib") || { fail "$abi: ar t exited non-zero"; continue; }
    lookup 'unwind-dw2-fde\.o' "$members"; rc=$?
    [ $rc -eq 0 ] || { fail "$abi: unwind-dw2-fde.o not found in $lib (lookup $rc)"; continue; }
    mkdir -p "$work/$abi" || { fail "$abi: mkdir"; continue; }
    (cd "$work/$abi" && "$AR" x "$lib" unwind-dw2-fde.o) || { fail "$abi: ar x exited non-zero"; continue; }
    o=$work/$abi/unwind-dw2-fde.o
    syms=$("$NM" "$o") || { fail "$abi: nm exited non-zero"; continue; }
    dis=$("$OBJDUMP" -d "$o") || { fail "$abi: objdump exited non-zero"; continue; }
    names=$("$AWK" '{print $NF}' <<<"$syms") || { fail "$abi: awk exited non-zero"; continue; }
    for s in registered_frames registered_objects; do
        lookup "$s" "$names"; case $? in 0) ;; 1) fail "$abi: b-tree symbol $s missing (list path still built?)" ;; *) fail "$abi: could not judge $s" ;; esac
    done
    for s in seen_objects unseen_objects release_registered_frames; do
        lookup "$s" "$names"; case $? in 1) ;; 0) fail "$abi: $s present (list path, or the trees destroyed at exit)" ;; *) fail "$abi: could not judge $s" ;; esac
    done
    if [ $abi = ilp32 ]; then want=lwarx; not=ldarx; else want=ldarx; not=lwarx; fi
    grep -qw -- "$want" <<<"$dis"; case $? in 0) ;; 1) fail "$abi: no $want in the registry's atomics" ;; *) fail "$abi: could not judge $want" ;; esac
    n_not=$(grep -cw -- "$not" <<<"$dis"); [ $? -le 1 ] || fail "$abi: could not count $not"
    [ $abi_bad -eq 0 ] && echo "libgcc-fde-fast-path: ok $abi $lib: b-tree registry, no list path, no tree destructor, $want present ($not x$n_not)"
done
[ $status -eq 0 ] && echo "PASS: libgcc-fde-fast-path-test"
exit $status

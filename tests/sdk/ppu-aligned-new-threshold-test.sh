#!/usr/bin/env bash
# Codegen guard for GCC patch 0083 (cell64lv2): __STDCPP_DEFAULT_NEW_ALIGNMENT__
# is 16 -- the target's biggest alignment in bytes -- and a type whose
# alignof is 16 (or less) dispatches to plain operator new, not the
# aligned (placement) form.
#
# Before 0083 the C++ aligned-new threshold was 8: the compiler derives
# __STDCPP_DEFAULT_NEW_ALIGNMENT__ as
#     aligned_new_threshold = malloc_alignment() / BITS_PER_UNIT
#   = MAX(max_align_t_align(), MALLOC_ABI_ALIGNMENT) / BITS_PER_UNIT
# and rs6000.h fixes MALLOC_ABI_ALIGNMENT at 64 bits, so the threshold
# came out 8.  The ordinary _mallocr on this target returns 16-byte-
# aligned blocks (newlib patch 0015 sets MALLOC_ALIGNMENT to 16, and
# the size/split math -- chunk2mem is aligned at heap acquisition and
# sizes/splits are rounded, realloc included -- preserves that
# alignment), so before 0083 every `new` of a type holding a vector
# or any 16-byte-aligned field called the aligned operator new even
# though the ordinary allocator already served a 16-byte-aligned
# block.  0083 raises MALLOC_ABI_ALIGNMENT to 128 bits in the target
# header, so the threshold becomes 16 and types with alignof <= 16
# use plain operator new; only types with alignof > 16 (exceeding the
# target biggest) still take the aligned form.
#
# Note: vec_malloc is a separate API (calls _memalign_r(...,16,...)).
# vec_mallocr's MINSIZE is sizeof(struct malloc_chunk) (32 under LP64),
# not a fixed 16.  Neither is the guarantee.  A custom malloc
# replacement must also return 16-byte-aligned blocks, because the
# compiler now assumes it for malloc/calloc/realloc/strdup/strndup.
#
# The rows:
#   (1) predefine row: powerpc64-ps3-elf-g++ (both data models) reports
#       __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 16;
#   (2) asm row: a struct V with alignas(16) members -- the exact shape
#       of an AltiVec/vmx vector-holder -- `new V()` at -O/-O2/-O3
#       references plain operator new and NOT the aligned form.  The
#       mangled size_t is model-dependent: j in ILP32 (sizeof(long)==4,
#       sizeof(size_t)==4), m in LP64; the aligned form appends
#       St11align_val_t as the second parameter.
#   (3) control (red if 0083 regresses the dispatch): the same struct
#       but a second field at alignas(32) -- EXCEEDING the target
#       biggest -- still takes the aligned form; without this control a
#       threshold set to 32 or higher would also pass the green row,
#       but it would mis-dispatch 16-aligned types to plain new only by
#       accident.  0083 keeps the threshold at exactly the target
#       biggest (16).
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-aligned-new-threshold-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-aligned-new-threshold: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cxx" ] || [ -x "$cxx.exe" ]; } || { echo "ppu-aligned-new-threshold: FAIL: no PPU C++ compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

# Green: a shape a PS3 vector-holder has (a 16-byte-aligned field).
#   alignas(16) doubles -- same shape as vector<double> /
#   vector<vec_float4>, the type the card calls out.
#   alignof(V) == 16, so with the threshold at 16, `new V` is plain new.
cat > "$work/green.cpp" <<'CXX'
struct V { alignas(16) double x; alignas(16) double y; };
V *make() { return new V(); }
CXX

# Red control: the same struct PLUS a field with alignas(32), so
#   alignof(V) == 32 > 16 (threshold).  This MUST still take the aligned
#   operator new; without this the green row could be satisfied by
#   setting the threshold to any value >=16 (including 32 or 64), which
#   is wrong.  The aligned form's mangled name carries St11align_val_t
#   (std::align_val_t is the second parameter).
cat > "$work/red.cpp" <<'CXX'
struct V { alignas(16) double x; alignas(16) double y; alignas(32) double h; };
V *make() { return new V(); }
CXX

status=0
for model in "" -mlp64; do
    label="${model:-ilp32}"
    for opt in -O -O2 -O3; do
        tag="$label $opt"

        # (1) predefine row: __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 16
        rm -f "$work/m.txt"
        if ! out=$("$cxx" $model -std=c++17 -dM -E -x c++ /dev/null -o "$work/m.txt" 2>&1); then
            echo "ppu-aligned-new-threshold: FAIL $tag: predefine compile"; printf '%s\n' "$out" | head -4; status=1; continue
        fi
        got=$(awk '$1=="#define" && $2=="__STDCPP_DEFAULT_NEW_ALIGNMENT__"{print $3; exit}' "$work/m.txt")
        if [ "$got" = "16" ]; then
            echo "ppu-aligned-new-threshold: ok   $tag predefine=16"
        else
            echo "ppu-aligned-new-threshold: FAIL $tag predefine (got: ${got:-absent})"; status=1
        fi

        # (2) green asm row: plain new, NOT the aligned form
        rm -f "$work/g.s"
        if ! out=$("$cxx" $model $opt -std=c++17 -S "$work/green.cpp" -o "$work/g.s" 2>&1); then
            echo "ppu-aligned-new-threshold: FAIL $tag: green compile"; printf '%s\n' "$out" | head -4; status=1; continue
        fi
        # size_t in the mangled operator new is model-dependent:
        # ILP32  sizeof(size_t)==4  ->  "j" (unsigned int)   _Znwj ...
        # LP64   sizeof(size_t)==8  ->  "m" (long, 64-bit)   _Znwm ...
        # The aligned form appends the second parameter's type name
        # St11align_val_t, so both _ZnwjSt11align_val_t and
        # _ZnwmSt11align_val_t exist depending on the model.
        plain=$(grep -cE '\b_Znw[jm]\b' "$work/g.s" 2>/dev/null); plain=${plain:-0}
        aligned_in_green=$(grep -cE '_Znw[jm]St11align_val_t' "$work/g.s" 2>/dev/null); aligned_in_green=${aligned_in_green:-0}
        if [ "$plain" -ge 1 ] && [ "$aligned_in_green" -eq 0 ]; then
            echo "ppu-aligned-new-threshold: ok   $tag green plain (plain=$plain aligned=$aligned_in_green)"
        else
            echo "ppu-aligned-new-threshold: FAIL $tag green (plain=$plain aligned=$aligned_in_green)"
            grep -nE '_Znw' "$work/g.s" | head -4 || true
            status=1
        fi

        # (3) red control: alignas(32) member STILL takes the aligned form
        rm -f "$work/h.s"
        aligned_in_red=0
        if out=$("$cxx" $model $opt -std=c++17 -S "$work/red.cpp" -o "$work/h.s" 2>&1); then
            aligned_in_red=$(grep -cE '_Znw[jm]St11align_val_t' "$work/h.s" 2>/dev/null); aligned_in_red=${aligned_in_red:-0}
            if [ "$aligned_in_red" -ge 1 ]; then
                echo "ppu-aligned-new-threshold: ok   $tag red control alignas(32) -> aligned new (aligned=$aligned_in_red)"
            else
                echo "ppu-aligned-new-threshold: FAIL $tag red control (alignas(32) should take aligned new; got aligned=$aligned_in_red)"
                status=1
            fi
        else
            # A compile failure is never a soft skip: the red control is
            # what keeps the threshold from drifting past the target
            # biggest, so the check has to be able to fail.
            echo "ppu-aligned-new-threshold: FAIL $tag red control (red.cpp did not compile)"
            printf '%s\n' "$out" | head -4 | sed 's/^/    /'
            status=1
        fi
    done
done

# Rejecting self-test: prove the red-control FAIL branch is reachable
# and that the script exits nonzero when it fires.  Build a stub
# compiler that fails only the -S red.cpp invocation (the row we
# want to reach the FAIL branch on) and succeeds the predefine row
# and the green row so those do not `continue` past the red row.
# The only assertion is on the red-control outcome:
#   - the script's exit code is nonzero;
#   - the last "red control" line is a FAIL naming the compile
#     failure -- NOT the "note ... skipped" soft-skip line that was
#     there before the FAIL branch was added.
# A red control that cannot reject is the same as no red control.
# Guarded by PPU_AN_NO_SELFTEST so the nested invocation does not
# itself run the self-test (no infinite recursion).
if [ -z "${PPU_AN_NO_SELFTEST:-}" ]; then
    stub="$(mktemp -d)"
    trap 'rm -rf "$work" "$stub"' EXIT
    mkdir -p "$stub/ppu/bin"
    cat > "$stub/ppu/bin/powerpc64-ps3-elf-g++" <<'SH'
#!/bin/sh
# POSIX sh.  Fail only the -S invocation whose source is red.cpp.
# Succeed the predefine (-dM) row and the green -S row.
# Order of checks: red.cpp (fail), -dM (emit predefine), -o (green).
for a in "$@"; do
    case "$a" in
        *red.cpp) echo "stub-red: not supported" >&2; exit 1 ;;
    esac
done
for a in "$@"; do
    [ "$a" = "-dM" ] && { echo '#define __STDCPP_DEFAULT_NEW_ALIGNMENT__ 16'; exit 0; }
done
out=""; prev=""
for a in "$@"; do
    if [ "$prev" = "-o" ]; then out="$a"; fi
    prev="$a"
done
[ -n "$out" ] && { echo 'call _Znwm' > "$out"; }
exit 0
SH
    chmod +x "$stub/ppu/bin/powerpc64-ps3-elf-g++"
    echo "ppu-aligned-new-threshold: self-test: forcing a red-control regression"
    out=$(PPU_AN_NO_SELFTEST=1 bash "${BASH_SOURCE[0]}" --ps3dev "$stub" 2>&1); rc=$?
red_line=$(printf '%s\n' "$out" | grep -E 'red control' | tail -n 1)
    if [ $rc -ne 0 ] \
       && printf '%s\n' "$red_line" | grep -qE 'FAIL .*red control \(red\.cpp did not compile\)'; then
        echo "ppu-aligned-new-threshold: ok   self-test: red-control FAIL branch fires and exits nonzero (rc=$rc)"
    elif [ -n "$red_line" ] && printf '%s\n' "$red_line" | grep -q 'note .*red control'; then
        echo "ppu-aligned-new-threshold: FAIL self-test: red-control was a SOFT SKIP (the FAIL branch is unreachable)"
        printf '%s\n' "$red_line" | sed 's/^/    /'
        status=1
    else
        echo "ppu-aligned-new-threshold: FAIL self-test: no red-control line in the self-test run (rc=$rc)"
        printf '%s\n' "$out" | tail -n 6 | sed 's/^/    /'
        status=1
    fi
fi

[ $status -eq 0 ] && echo "ppu-aligned-new-threshold: PASS"
exit $status

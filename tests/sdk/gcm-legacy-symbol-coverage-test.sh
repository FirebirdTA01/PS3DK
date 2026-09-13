#!/usr/bin/env bash
# <rsx/gcm_sys.h> declares the PSL1GHT-flavoured gcm* surface, and
# <cell/gcm.h>'s static inlines forward to it, so every one of those names
# has to be DEFINED somewhere or a caller gets "undefined reference to
# gcmXxx" at link time with nothing in the build having warned.  Issue #7
# was exactly that: 28 of the 75 declared names had no definition at all,
# because the definitions are hand-written shims in
# sdk/libgcm_sys_legacy/src/gcm_legacy_wrappers.c and that file was never
# reconciled against the header.  A declaration is not coverage.
#
# Four checks.  B and C need a built install tree and are skipped without
# one; A and D are host-only and always run.
#
#   A. every gcm* function declared in rsx/gcm_sys.h has a real BODY in
#      our SDK sources.  "Body" means the closing paren is followed by
#      '{' - a prototype is not a definition, and an earlier draft of this
#      check accepted a multi-line prototype as one.
#   B. every one of them is actually defined in the installed archives,
#      for BOTH ABIs.  This is the authoritative check: it reads real
#      symbol tables, not source text.
#   C. the ABI each merged object was compiled for, read out of the
#      ps3tc_abi_witness_ptr symbol's SIZE.  An ELF64 PPC64 object records
#      nothing else that distinguishes our ILP32 hybrid from -mlp64 -
#      same class, e_flags 0 both ways - which is how the lp64 archives
#      came to hold ILP32 objects storing 4-byte stw into 8-byte pointer
#      outputs.  Byte-comparing the two copies is NOT used here: two
#      objects differing does not prove either is LP64, and an
#      ABI-independent object (hand-written .S, say) can legitimately be
#      identical in both.
#   D. the build wiring that carries the ABI flag into those objects.
#      scripts/build-cell-stub-archives.sh builds each merged object once
#      per ABI; passing the flag as CFLAGS does not work, because a
#      `CFLAGS :=` line in a makefile overrides the environment value and
#      the flag vanishes silently.  Structural, not behavioural: it pins
#      the wiring, and C is what proves the result.
#
# Check A carries its own adversarial self-tests - a deleted definition
# and a definition replaced by a bare prototype - and must reject both.
# A coverage check that cannot fail proves nothing.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
header="$root/sdk/include/rsx/gcm_sys.h"
status=0

fail() { echo "gcm-legacy-symbol-coverage: FAIL: $*" >&2; status=1; }
note() { echo "gcm-legacy-symbol-coverage: $*"; }

# Names that rsx/gcm_sys.h declares and we deliberately do not define.
# Each entry needs a reason; an unexplained entry is a silenced bug.
#
#   gcmSetUserCommand - PSL1GHT binds this to firmware export 0x8bde5ebf,
#     which is FNID("cellGcmSetUserCommand"), but the reference SDK's own
#     libgcm_sys_stub.a does not export that symbol at all, and both the
#     reference headers and RPCS3's cellGcmSys implement
#     cellGcmSetUserCommand as a command-buffer emitter taking
#     (context, cause) - a different contract from the callback pointer
#     this header declares.  Implementing it means resolving that contract
#     first; guessing it would silently encode the wrong thing.
ALLOWLIST="gcmSetUserCommand"

# ---------------------------------------------------------------- helpers

# Every gcm* function name declared in the header.  Declarations wrap over
# several lines, so join first, then take the identifier before each '('.
declared_names() {
    sed -e 's://.*::' "$1" \
        | tr '\n' ' ' \
        | sed -e 's:/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/: :g' \
        | tr ';' '\n' \
        | grep -oE '\bgcm[A-Za-z0-9_]*[[:space:]]*\(' \
        | sed -e 's:[[:space:]]*(::' \
        | sort -u
}

# Every gcm* name our SDK sources give a body to.  Comments are stripped,
# the file is flattened, and a name counts only when its argument list is
# balanced and the next non-space character after it is '{'.  Prototypes
# end in ';' and are therefore not counted.
defined_in_sources() {
    local tree="$1"
    cat "$tree"/sdk/libgcm_sys_legacy/src/*.c \
        "$tree"/sdk/libgcm_cmd/src/*.c \
        "$tree"/sdk/librsx/src/*.c 2>/dev/null \
    | awk '
        { src = src $0 "\n" }
        END {
            n = length(src); i = 1; flat = ""
            while (i <= n) {
                two = substr(src, i, 2)
                if (two == "/*") {
                    j = index(substr(src, i + 2), "*/")
                    if (j == 0) break
                    i += j + 3; flat = flat " "; continue
                }
                if (two == "//") {
                    while (i <= n && substr(src, i, 1) != "\n") i++
                    flat = flat " "; continue
                }
                c = substr(src, i, 1)
                flat = flat (c == "\n" ? " " : c)
                i++
            }
            while (match(flat, /gcm[A-Za-z0-9_]*[ \t]*\(/)) {
                name = substr(flat, RSTART, RLENGTH)
                sub(/[ \t]*\($/, "", name)
                rest = substr(flat, RSTART + RLENGTH)
                depth = 1; k = 1; m = length(rest)
                while (k <= m && depth > 0) {
                    c = substr(rest, k, 1)
                    if (c == "(") depth++
                    else if (c == ")") depth--
                    k++
                }
                if (depth == 0) {
                    tail = substr(rest, k)
                    sub(/^[ \t]+/, "", tail)
                    if (substr(tail, 1, 1) == "{") print name
                }
                flat = substr(flat, RSTART + RLENGTH)
            }
        }' \
    | sort -u
}

# ------------------------------------------------- A. source-level coverage

check_sources() {
    local tree="$1" quiet="${2:-}"
    local decl def missing="" n
    decl=$(declared_names "$tree/sdk/include/rsx/gcm_sys.h")
    def=$(defined_in_sources "$tree")
    for n in $decl; do
        case " $ALLOWLIST " in *" $n "*) continue;; esac
        printf '%s\n' "$def" | grep -qx "$n" || missing="$missing $n"
    done
    if [ -n "$missing" ]; then
        [ -n "$quiet" ] || echo "declared but never defined:$missing" >&2
        return 1
    fi
    return 0
}

if check_sources "$root"; then
    note "A ok: every declared gcm* has a body in sdk sources"
else
    fail "rsx/gcm_sys.h declares gcm* functions with no definition (see above)"
fi

# Self-tests.  Two mutants, each the exact shape of a real mistake: the
# definition deleted, and the definition demoted to a bare prototype.
selftest_root=$(mktemp -d)
trap 'rm -rf "$selftest_root"' EXIT

make_mutant() {   # $1 = subdir name, $2 = sed script applied to the wrappers
    local dir="$selftest_root/$1"
    mkdir -p "$dir/sdk/include/rsx" "$dir/sdk/libgcm_sys_legacy/src" \
             "$dir/sdk/libgcm_cmd/src" "$dir/sdk/librsx/src"
    cp "$header" "$dir/sdk/include/rsx/gcm_sys.h"
    cp "$root/sdk/libgcm_sys_legacy/src/"*.c "$dir/sdk/libgcm_sys_legacy/src/"
    cp "$root/sdk/libgcm_cmd/src/"*.c        "$dir/sdk/libgcm_cmd/src/"
    cp "$root/sdk/librsx/src/"*.c            "$dir/sdk/librsx/src/" 2>/dev/null || true
    sed -i -e "$2" "$dir/sdk/libgcm_sys_legacy/src/gcm_legacy_wrappers.c"
    printf '%s' "$dir"
}

# 1. definition removed outright.
d=$(make_mutant deleted 's:^s32 gcmInitDefaultFifoMode(s32 mode)$:s32 REMOVED_BY_SELFTEST(s32 mode):')
grep -q 'REMOVED_BY_SELFTEST' "$d/sdk/libgcm_sys_legacy/src/gcm_legacy_wrappers.c" \
    || fail "self-test 1 did not modify the source; the result below proves nothing"
if check_sources "$d" quiet; then
    fail "self-test 1: check A accepted a tree with gcmInitDefaultFifoMode removed"
else
    note "A self-test 1 ok: a removed definition is rejected"
fi

# 2. definition replaced by a prototype - the mutant that defeated the
#    first draft of this check, which only looked for a line not ending
#    in ';' and so counted a wrapped prototype as a body.
d=$(make_mutant prototype 's:^s32 gcmInitDefaultFifoMode(s32 mode)$:s32 gcmInitDefaultFifoMode(s32 mode)\n;\nstatic void selftest_unused(void) {:')
if check_sources "$d" quiet; then
    fail "self-test 2: check A accepted a bare prototype as a definition"
else
    note "A self-test 2 ok: a prototype is not counted as a definition"
fi

# ------------------------------------------------------- D. ABI-flag wiring

script="$root/scripts/build-cell-stub-archives.sh"
# Anchored so ABI_CFLAGS="$cc_flags" - the fix - does not match the bug.
if grep -qE '(^|[^A-Z_])CFLAGS="\$cc_flags"' "$script"; then
    fail "build-cell-stub-archives.sh passes the ABI flag as CFLAGS; a makefile CFLAGS := assignment overrides it (use ABI_CFLAGS)"
fi
# Producer side.  Checking only that the OLD spelling is gone leaves the
# script free to pass no ABI flag at all, so assert it positively: join the
# backslash continuations, then every `make -C` that builds a merged object
# must carry ABI_CFLAGS on that same command.
fed=0
while IFS= read -r cmd; do
    case "$cmd" in
        *"make -C"*)
            fed=$((fed + 1))
            case "$cmd" in
                *'ABI_CFLAGS="$cc_flags"'*) ;;
                *) fail "build-cell-stub-archives.sh runs a merged-object build with no ABI flag: $cmd" ;;
            esac
            ;;
    esac
done <<EOF
$(sed -e ':a' -e '/\\$/{N;s/\\\n//;ba' -e '}' "$script" | tr -s ' \t' ' ')
EOF
[ "$fed" -gt 0 ] || fail "check D found no merged-object make invocations to verify"
for mk in sdk/libgcm_sys_legacy sdk/libio_legacy sdk/libc_stub_extras \
          sdk/libfiber_stub_extras sdk/libusb_legacy; do
    [ -f "$root/$mk/Makefile" ] || continue
    grep -qE '^CFLAGS[[:space:]]*\+=[[:space:]]*\$\(ABI_CFLAGS\)' "$root/$mk/Makefile" \
        || fail "$mk/Makefile does not fold \$(ABI_CFLAGS) into CFLAGS; its lp64 object will be built ILP32"
done
[ "$status" -eq 0 ] && note "D ok: the ABI flag reaches every merged-object makefile"

# --------------------------------------------- B/C. installed-archive checks

ps3dk="${PS3DK:-}"
[ -n "$ps3dk" ] || ps3dk="${PS3DEV:+$PS3DEV/ps3dk}"

# Binutils live in different places depending on how the tree was built:
# a packaged Windows install consolidates them under $PS3DK/ppu/bin, while
# a native build leaves them in $PS3DEV/ppu/bin (which is what
# build-cell-stub-archives.sh itself invokes).  Search both, then PATH,
# so a valid toolchain is never reported as missing.
find_tool() {   # $1 = tool basename (nm, ar)
    local dir cand
    for dir in "$ps3dk/ppu/bin" "${PS3DEV:-}/ppu/bin" "${PPU_PREFIX:-}/bin"; do
        [ -n "$dir" ] || continue
        for cand in "$dir/powerpc64-ps3-elf-$1" "$dir/powerpc64-ps3-elf-$1.exe"; do
            [ -x "$cand" ] && { printf '%s' "$cand"; return 0; }
        done
    done
    cand=$(command -v "powerpc64-ps3-elf-$1" 2>/dev/null) \
        && [ -n "$cand" ] && { printf '%s' "$cand"; return 0; }
    return 1
}

if [ -z "$ps3dk" ] || [ ! -d "$ps3dk/ppu/lib" ]; then
    note "B/C skipped: no installed PPU tree (set PS3DK to a built install to run them)"
    exit $status
fi
# Derive each tool independently.  Deriving ar from the nm path by string
# surgery breaks on the .exe suffix and then silently finds no members.
nm=$(find_tool nm) || { fail "no powerpc64-ps3-elf-nm under $ps3dk/ppu/bin"; exit $status; }
ar=$(find_tool ar) || { fail "no powerpc64-ps3-elf-ar under $ps3dk/ppu/bin"; exit $status; }

# PPC64 ELFv1 puts function symbols in .opd, so nm reports them as 'D',
# not 'T' - accept any defined type.  But EXTERNAL only: a local symbol of
# the right name sits in the archive and satisfies a nm grep while the
# linker still refuses it, so `nm --defined-only` alone (which lists
# locals too, in lowercase) would call a genuinely unlinkable archive
# covered.  -g is what makes this check mean "a caller can bind to it".
archive_syms() {
    "$nm" -g --defined-only "$@" 2>/dev/null \
        | awk '$2 ~ /^[A-Z]$/ {print $3}' | sort -u
}

for abi in ilp32 lp64; do
    if [ "$abi" = ilp32 ]; then libdir="$ps3dk/ppu/lib"; else libdir="$ps3dk/ppu/lib/lp64"; fi
    [ -d "$libdir" ] || { note "B skipped for $abi: no $libdir"; continue; }
    set --
    for l in libgcm_sys.a libgcm_cmd.a librsx.a; do
        [ -f "$libdir/$l" ] && set -- "$@" "$libdir/$l"
    done
    [ $# -gt 0 ] || { note "B skipped for $abi: no gcm archives in $libdir"; continue; }
    syms=$(archive_syms "$@")
    missing=""
    for n in $(declared_names "$header"); do
        case " $ALLOWLIST " in *" $n "*) continue;; esac
        printf '%s\n' "$syms" | grep -qx "$n" || missing="$missing $n"
    done
    if [ -n "$missing" ]; then
        fail "$abi archives do not define:$missing"
    else
        note "B ok ($abi): all declared gcm* resolve in the installed archives"
    fi
done

# C. pointer width the merged wrapper object was actually compiled with.
# nm -S prints the size as fixed-width hex.  Strip the leading zeros in
# sed rather than converting in awk: strtonum() is a gawk extension and
# this has to run under mawk on a stock Debian/Ubuntu host too.
witness_width() {   # $1 = archive path
    "$nm" -S --defined-only "$1" 2>/dev/null \
        | awk '$NF == "ps3tc_abi_witness_ptr" { print $2; exit }' \
        | sed -e 's:^0*::'
}
for abi in ilp32 lp64; do
    if [ "$abi" = ilp32 ]; then
        lib="$ps3dk/ppu/lib/libgcm_sys.a"; want=4
    else
        lib="$ps3dk/ppu/lib/lp64/libgcm_sys.a"; want=8
    fi
    [ -f "$lib" ] || { note "C skipped for $abi: no $lib"; continue; }
    got=$(witness_width "$lib")
    if [ -z "$got" ]; then
        fail "C ($abi): $lib carries no ps3tc_abi_witness_ptr; rebuild the legacy wrappers"
    elif [ "$got" != "$want" ]; then
        fail "C ($abi): $lib was compiled with ${got}-byte pointers, expected $want (the ABI flag did not reach the compiler)"
    else
        note "C ok ($abi): merged wrapper object compiled with ${got}-byte pointers"
    fi
done

exit $status

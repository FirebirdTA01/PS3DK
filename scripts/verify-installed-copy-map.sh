#!/usr/bin/env bash
# verify-installed-copy-map.sh  (t_cab4a49d -- verification helper)
#
# Drive the two producers of the REPLACING log lines against a SCRATCH
# prefix (never the shared SDK install), tee all output to
# <prefix>/verify.log, then grep the nine expected REPLACING strings and
# exit 0 only if all nine are present >= 1 (exit 1 if any are missing).
# Exit 2 on misuse (prefix is the shared SDK or under it, or an arg is
# missing).
#
# This script does NOT call git, does NOT call version.sh, and does NOT
# require the repo to be a git checkout -- it runs from a plain directory
# (a linked Windows worktree, say), where git commands cannot run.  The
# make targets it invokes (install-headers, librsx install, libgcm_cmd
# install) do not call version.sh, so they are safe under this constraint.
#
# usage:
#   verify-installed-copy-map.sh <wslnative source-tree> <scratch prefix> <toolchain prefix>
#
#   <wslnative source-tree>  the PS3_SDK_ROOT -- the directory containing
#                            runtime/lv2/, sdk/librsx/, sdk/libgcm_cmd/,
#                            sdk/Makefile (the top of the tree)
#   <scratch prefix>         a fresh writable directory.  MUST NOT be (or be
#                            under) the shared C:/SDKs/Sony/homebrew/PS3DK
#                            install -- the guard below exits 2 if so.  Also
#                            the container for the toolchain copy: arg3
#                            below must live UNDER this (strictly a
#                            subdirectory), because build-runtime-lv2.sh
#                            sources env.sh and thus
#                            force-sets PS3DK=$PS3DEV/ps3dk, so the librt
#                            install AND the lv2-sprx / lv2-crti / lv2-crt0
#                            / lv2.ld / lv2-prx-* startfile writes land
#                            INSIDE arg3.  Pointing arg3 at the real WSL
#                            toolchain that other gates use would rewrite
#                            its startfiles -- that is the P1 bug this
#                            guard exists to prevent.  Make a scratch-local
#                            copy first:
#                                cp -a <real toolchain> $SCRATCH/toolchain
#   <toolchain prefix>       the prefix that has ppu/bin/powerpc64-ps3-elf-gcc.
#                            MUST be a strict subdirectory of <scratch
#                            prefix> (e.g. $SCRATCH/toolchain).
#
# Exit codes:
#   0   all 9 REPLACING strings present
#   1   one or more of the 9 REPLACING strings missing from the log
#   2   misuse (bad args, a prefix collides with the shared SDK, or the
#       toolchain prefix is not a strict subdirectory of the scratch prefix)

set -uo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: verify-installed-copy-map.sh <wslnative source-tree> <scratch prefix> <toolchain prefix>" >&2
    exit 2
fi

SRC="$1"; SCRATCH="$2"; TCHAIN="$3"
LOG="$SCRATCH/verify.log"

# ---------------------------------------------------------------------------
# Guard: refuse to point PS3DK at the shared SDK install or any subpath of
# it.  Covers both C:\ and /mnt/c forms (WSL mount) and also the "under"
# case (the shared prefix plus any additional directory suffix).
# ---------------------------------------------------------------------------
normalize() {  # strip trailing slash; accept either sep
    local p="$1"
    p="${p%/}"
    p="${p//\\//}"
    printf '%s' "$p"
}

# shared_roots is test-injectable: PS3TC_SHARED_ROOTS lists ADDITIONAL roots
# to enforce (so a host test can alias a scratch-local stand-in alongside
# the real C:/SDKs roots).  It only ADDS entries never remove; the
# production defaults below always apply in addition.
if [[ -n "${PS3TC_SHARED_ROOTS:-}" ]]; then
    read -a shared_roots_extra <<< "${PS3TC_SHARED_ROOTS}"
    shared_roots=(
        '/c/SDKs/Sony/homebrew/PS3DK'
        '/mnt/c/SDKs/Sony/homebrew/PS3DK'
        '/c/SDKs/Sony/Homebrew/PS3DK'
        '/mnt/c/SDKs/Sony/Homebrew/PS3DK'
        "${shared_roots_extra[@]}"
    )
else
    shared_roots=(
        '/c/SDKs/Sony/homebrew/PS3DK'
        '/mnt/c/SDKs/Sony/homebrew/PS3DK'
        '/c/SDKs/Sony/Homebrew/PS3DK'
        '/mnt/c/SDKs/Sony/Homebrew/PS3DK'
    )
fi

# Both arg2 (scratch) AND arg3 (toolchain) must clear the guard: the 7
# make-producer lines write under arg2, and the 2 librt lines (env.sh
# force-sets PS3DK=$PS3DEV/ps3dk, see below) write under arg3/ps3dk.  For
# each dir we compare dir itself and dir/ps3dk against the shared roots.
#
# The comparison is case-insensitive because /c/ paths are Windows-mounted
# and PS3DK vs ps3dk are the same directory there -- that is the specific
# "parent-of-PS3DK-as-toolchain" danger we must refuse.
#
# The comparison runs on BOTH the raw dir and its realpath -m resolution,
# because a raw string can pass the check while the resolved path is the
# shared root (the reverse of the "..." escape).  A scratch-local symlink
# that points INTO the shared PS3DK is exactly that case and must fail.
ci_eq() {  # ci_eq <a> <b>  ->  true iff a == b in a case-insensitive way
    local a="${1%/}" b="${2%/}"
    [[ "${a,,}" == "${b,,}" || "${a,,}" == "${b,,}"/* || "${b,,}" == "${a,,}"/* ]]
}
collides_with_shared() {  # collides_with_shared <dir>
    local d="$1" rd r2
    rd="$(realpath -m -- "$d" 2>/dev/null || true)"
    for root in "${shared_roots[@]}"; do
        if ci_eq "$d" "$root"          || ci_eq "${d}/ps3dk" "$root" ||
           ci_eq "${rd:-$d}" "$root"   || ci_eq "${rd:-$d}/ps3dk" "$root"; then
            return 0
        fi
    done
    return 1
}
guard_dir() {
    local label="$1" dir="$2"
    local dn
    local resolved
    dn="$(normalize "$dir")"
    resolved="$(realpath -m -- "$dir" 2>/dev/null)" || resolved="<unresolvable>"
    if collides_with_shared "$dir"; then
        echo "verify-installed-copy-map: REFUSE -- $label collides with the shared PS3DK install"
        echo "  raw     : $dn   (its /ps3dk: ${dn}/ps3dk)"
        echo "  resolved: $resolved"
        echo "  the live run must never touch the shared SDK, including via a symlink alias; use a scratch/writable dir."
        exit 2
    fi
}
guard_dir "scratch prefix (arg2)" "$SCRATCH"
guard_dir "toolchain prefix (arg3)" "$TCHAIN"

# The librt producer (build-runtime-lv2.sh) sources env.sh, which force-sets
# PS3DK=$PS3DEV/ps3dk, AND writes the lv2 startfile objects into the gcc
# dir under $PS3DEV.  Both of those fall INSIDE arg3.  If arg3 is the real
# WSL toolchain that other gates build against, this verification run would
# rewrite its startfiles.  Constrain arg3 to a disposable copy contained in
# arg2 so nothing outside the scratch tree can change.
#
# A raw string-prefix check is NOT enough: `$SCRATCH/../real-toolchain`
# passes the "scratch/"-prefix test while escaping the scratch, and a
# scratch-local SYMLINK that points at the real toolchain also passes the
# string check while the writes land outside.  Resolve both via realpath
# (-m so it works on paths that do not yet exist, which is what we want
# here for arg3 = $SCRATCH/toolchain before it has been created) and
# re-do the containment check on the RESOLVED forms.
resolved_scratch="$(realpath -m -- "$SCRATCH" 2>/dev/null || true)"
resolved_tchain="$(realpath -m -- "$TCHAIN" 2>/dev/null || true)"
if [[ -z "$resolved_scratch" || -z "$resolved_tchain" ]]; then
    echo "verify-installed-copy-map: REFUSE -- realpath failed to resolve one of the prefixes"
    echo "  scratch : $SCRATCH  (resolved: ${resolved_scratch:-<empty>})"
    echo "  toolchain: $TCHAIN  (resolved: ${resolved_tchain:-<empty>})"
    exit 2
fi
if ! [[ "$resolved_tchain" == "${resolved_scratch}/"* ]]; then
    echo "verify-installed-copy-map: REFUSE -- resolved toolchain prefix is not strictly under the resolved scratch prefix"
    echo "  raw    : scratch=$SCRATCH  toolchain=$TCHAIN"
    echo "  resolved: scratch=$resolved_scratch"
    echo "            toolchain=$resolved_tchain"
    echo "  this covers '..' escapes (\$SCRATCH/../tc) and scratch-local symlinks pointing outside the scratch."
    echo "  make a disposable copy under the scratch first:  cp -a <real toolchain> $SCRATCH/toolchain"
    exit 2
fi

# The 7 make-based producers use bare tool names (powerpc64-ps3-elf-gcc,
# powerpc64-ps3-elf-ar) and install via PS3DEV/PS3DK.  Put the toolchain's
# bin dirs on PATH and pin PS3DEV/PS3DK per invocation.
export PATH="${TCHAIN%/}/ppu/bin:${TCHAIN%/}/bin:${PATH:-}"

# Source directories must exist.
[[ -d "$SRC/runtime/lv2"        ]] || { echo "source tree missing runtime/lv2/ at $SRC/runtime/lv2"; exit 2; }
[[ -d "$SRC/sdk/librsx"         ]] || { echo "source tree missing sdk/librsx/ at $SRC/sdk/librsx"; exit 2; }
[[ -d "$SRC/sdk/libgcm_cmd"     ]] || { echo "source tree missing sdk/libgcm_cmd/ at $SRC/sdk/libgcm_cmd"; exit 2; }
[[ -x "$TCHAIN/ppu/bin/powerpc64-ps3-elf-gcc" ]] || { echo "toolchain missing ppu/bin/powerpc64-ps3-elf-gcc at $TCHAIN/ppu/bin/powerpc64-ps3-elf-gcc"; exit 2; }

mkdir -p "$SCRATCH" || { echo "cannot create scratch dir: $SCRATCH"; exit 2; }
: > "$LOG" || { echo "cannot write log: $LOG"; exit 2; }

# ---------------------------------------------------------------------------
# Run the two producers, teeing their output to $LOG.  We use a small
# sub-function so we can cleanly append with markers and preserve the exit
# code of each command.
# ---------------------------------------------------------------------------
# Step failures are tracked globally: any non-zero exit from a producer
# sets STEP_FAILED, which the final verdict checks even if all nine
# REPLACING strings are present (e.g. producer prints the lines and then
# fails its install).  We still keep going so the log has complete
# output for diagnosis.
STEP_FAILED=0
STEP_FAILED_LIST=()
run_step() {
    local title="$1"; shift
    printf '\n\n=== %s ===\n' "$title" >> "$LOG"
    echo "[verify] $title"
    if "$@" >> "$LOG" 2>&1; then
        echo "[verify]   ok"
    else
        local ec=$?
        echo "[verify]   FAILED (exit $ec) -- see $LOG"
        echo "      step: $title" >> "$LOG"
        STEP_FAILED=1
        STEP_FAILED_LIST+=("$title")
    fi
}

# Two producers, two different prefixes:
#  * librt (2 lines)  -- build-runtime-lv2.sh.  It sources env.sh, which
#    force-sets PS3DK=PS3DEV/ps3dk, so the artifact lands under
#    "$TCHAIN/ps3dk/ppu/lib{,/lp64}" (guard 2 covered that).
#  * librsx / libgcm_cmd / headers (7 lines) -- the make targets honour a
#    PS3DK override, so they land under "$SCRATCH/ppu/..."
#  * the 3 header lines are emitted by BOTH (build-runtime-lv2.sh also runs
#    `make install-headers`, and we run it standalone as well); the audit
#    only needs >= 1.
#  No git / version.sh on either path: build-runtime-lv2.sh and the three
#  make targets do not invoke version.sh (verified), so the plain (non-git)
#  linked worktree constraint holds.

run_step "librt both ABIs -- scripts/build-runtime-lv2.sh (2 REPLACING lines)" \
    env PS3DEV="$TCHAIN" PS3_TOOLCHAIN_ROOT="$SRC" \
        bash "$SRC/scripts/build-runtime-lv2.sh"
run_step "3 overriding headers -- sdk/ install-headers (under scratch prefix)" \
    env PS3DEV="$TCHAIN" PS3DK="$SCRATCH" \
        make -C "$SRC/sdk" install-headers
run_step "librsx.a both ABIs -- sdk/librsx/Makefile install (2 lines, scratch)" \
    env PS3DEV="$TCHAIN" PS3DK="$SCRATCH" \
        make -C "$SRC/sdk/librsx" install
run_step "libgcm_cmd.a both ABIs + own headers -- sdk/libgcm_cmd/Makefile install (2 lines, scratch)" \
    env PS3DEV="$TCHAIN" PS3DK="$SCRATCH" \
        make -C "$SRC/sdk/libgcm_cmd" install

# ---------------------------------------------------------------------------
# Grep audit: all 9 REPLACING strings must be present (>= 1 each).
# Each row: <installed path>  <expected source>
# ---------------------------------------------------------------------------
check() {
    local path="$1" src="$2"
    local count
    count=$(grep -c -F "REPLACING ${path} with ${src} " "$LOG" 2>/dev/null || true)
    count=${count:-0}
    if [[ "$count" -ge 1 ]]; then
        echo "  ok   [$count]  $path  (src $src)"
    else
        echo "  MISS [ 0]  $path  (src $src)"
    fi
    [[ "$count" -ge 1 ]]
}

echo
echo "audit against $LOG"
fail=0
check "ppu/lib/librt.a"                       "runtime/lv2/librt"  || fail=1
check "ppu/lib/lp64/librt.a"                  "runtime/lv2/librt"  || fail=1
check "ppu/lib/librsx.a"                      "sdk/librsx"         || fail=1
check "ppu/lib/lp64/librsx.a"                 "sdk/librsx"         || fail=1
check "ppu/lib/libgcm_cmd.a"                  "sdk/libgcm_cmd"     || fail=1
check "ppu/lib/lp64/libgcm_cmd.a"             "sdk/libgcm_cmd"     || fail=1
check "ppu/include/rsx/rsx_function_macros.h" "sdk/include/rsx"    || fail=1
check "ppu/include/rsx/gcm_sys.h"             "sdk/include/rsx"    || fail=1
check "ppu/include/net/socket.h"              "sdk/include/net"    || fail=1

# Verdict: PASS only if (a) no producer step failed AND (b) all 9 REPLACING
# lines are present.  A step that prints its lines and then fails its
# install is exactly the case (a) catches -- all 9 can be present while a
# step's exit was non-zero.
if [[ "$STEP_FAILED" -eq 1 ]]; then
    echo
    echo "FAIL -- one or more producer steps exited non-zero (the REPLACING lines may or may not be present):"
    for t in "${STEP_FAILED_LIST[@]}"; do
        echo "  failed step: $t"
    done
    echo "  see $LOG"
    exit 1
fi
if [[ "$fail" -eq 0 ]]; then
    echo
    echo "PASS -- all producers succeeded and all 9 REPLACING lines present in $LOG"
    exit 0
fi
echo
echo "FAIL -- one or more REPLACING lines missing (see $LOG)"
exit 1

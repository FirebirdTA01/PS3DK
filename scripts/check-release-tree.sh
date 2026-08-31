#!/usr/bin/env bash
# Gate an extracted (or staged) release tree against what the release claims.
#
# WHY THIS EXISTS
# ---------------
# Three v0.11.0 cuts shipped past their own checks before this script existed:
#
#   1. A re-package without build-sdk.sh produced a zip named v0.11.0 whose
#      VERSION and cell/sdk_version.h still read v0.10.732.  Presence checks
#      all passed; the cover was right and the inside was wrong.
#   2. The next cut fixed the stamps but skipped build-runtime-lv2.sh, so
#      librt.a did not contain the socket wrappers the CHANGELOG described.
#      Version stamps prove build-sdk.sh ran.  They prove NOTHING about
#      whether a given fix is in a given archive.
#   3. release.yml validated Windows host tools against a list frozen inside
#      the workflow, which still demanded the Python files v0.11.0 removed on
#      purpose -- so the release build failed BECAUSE the staging was correct.
#
# Every section below is a check that one of those would have failed.  If you
# are tempted to delete one because it looks redundant, the comment above it
# names the cut that shipped without it.
#
# Presence lists are NOT written here.  They are read from
# cmake/ps3-required-artifacts.txt, the same manifest used by
# cmake/ps3-self.cmake, scripts/package-windows-release.sh and
# .github/workflows/release.yml.  Hand-listing them a fourth time is exactly
# the drift that caused (3).
#
# Usage:
#   scripts/check-release-tree.sh <tree> [--version vX.Y.Z] [--manifest <path>]
#
# Environment:
#   PS3_NM   cross nm to use.  Otherwise resolved from PATH, then from the
#            tree itself when running on Windows.  Never silently skipped:
#            the symbol checks are the only ones that can catch (2).
set -uo pipefail

ROOT=""
WANT=""
MANIFEST=""
REQUIRE_ON_TAG=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)    WANT="${2:-}";     shift 2 ;;
        --version=*)  WANT="${1#*=}";    shift ;;
        --manifest)   MANIFEST="${2:-}"; shift 2 ;;
        --manifest=*) MANIFEST="${1#*=}"; shift ;;
        --require-on-tag) REQUIRE_ON_TAG=1; shift ;;
        -h|--help)    sed -n '2,36p' "$0"; exit 0 ;;
        *)            ROOT="$1";         shift ;;
    esac
done

# Compare like with like.  version.sh emits "v0.11.3+dirty" off a tag, while
# the values read out of the tree are matched with a vX.Y.Z pattern -- so a
# raw string compare rejected every dev cut for a difference in suffix rather
# than in version.  Normalise the expectation the same way the file values are
# read.
WANT_RAW="$WANT"
if [[ -n "$WANT" ]]; then
    WANT="$(grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' <<<"$WANT" | head -1)"
fi
[[ -n "$ROOT" ]] || { echo "usage: $0 <extracted-sdk-root> [--version vX.Y.Z]" >&2; exit 2; }
[[ -d "$ROOT" ]] || { echo "not a directory: $ROOT" >&2; exit 2; }

if [[ -z "$MANIFEST" ]]; then
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    MANIFEST="$script_dir/../cmake/ps3-required-artifacts.txt"
fi
[[ -f "$MANIFEST" ]] || { echo "manifest not found: $MANIFEST" >&2; exit 2; }

fail=0
ok()  { printf "  ok      %s\n" "$1"; }
bad() { printf "  FAIL    %s\n" "$1"; fail=$((fail+1)); }

# ---------------------------------------------------------------------------
# Symbols that must be provably present in specific binaries.
#
# This is the table to edit when a release claims a fix that a file-exists
# check cannot see.  Keeping it here rather than inline in section 4 means the
# next such fact is one line, not a new code block -- the goal being that
# nobody skips adding it because adding it looked like work.
#
# Format:  <path relative to the tree>|<symbols, space separated>|<mode>
#   defined  the archive must DEFINE the symbol (nm --defined-only)
#   any      the symbol need only appear (nm), e.g. an anchor in a crt object
#
# Provenance of the current rows:
#   librt.a          v0.11.0 implements getpeername/getsockname/select via
#                    Lv-2 syscalls 703/704/716.  A cut that skipped
#                    build-runtime-lv2.sh shipped stale archives whose version
#                    stamps still looked correct.
#   lv2-prx-crt.o    without __sys_prx_module_info the crt cannot anchor a
#                    PRX; the file existing says nothing about that.
#   librt.a pthread  v0.12.0 ships the pthread shim inside librt.a (pthread.o);
#                    trylock is the member carrying the EDEADLK->EBUSY fold,
#                    and a cut whose librt.a predates the shim would still
#                    carry correct-looking version stamps.  libpthread.a is a
#                    linker script (INPUT(-lrt)) -- presence-checked via the
#                    manifest, never nm'd.
# ---------------------------------------------------------------------------
REQUIRED_SYMBOLS=(
    "ppu/lib/librt.a|getpeername getsockname select|defined"
    "ppu/lib/lp64/librt.a|getpeername getsockname select|defined"
    "ppu/lib/librt.a|pthread_mutex_init pthread_mutex_trylock pthread_create pthread_once pthread_key_create pthread_cond_wait|defined"
    "ppu/lib/lp64/librt.a|pthread_mutex_init pthread_mutex_trylock pthread_create pthread_once pthread_key_create pthread_cond_wait|defined"
    "ppu/lib/lv2-prx-crt.o|__sys_prx_module_info|any"
    "ppu/lib/lp64/lv2-prx-crt.o|__sys_prx_module_info|any"
)

echo "checking $ROOT${WANT:+ (expecting $WANT)}"
echo "manifest $MANIFEST"

# ---------------------------------------------------------------------------
# 1. Presence, from the manifest.  Rows are counted so a parse failure cannot
#    pass vacuously -- a loop that runs zero times must not look clean.
# ---------------------------------------------------------------------------
echo "-- required artifacts (from the manifest) --"
checked=0
present=0
while read -r category path target; do
    case "$category" in
        host_bin|host_file|ppu_stub|sdk_core|spu_lib|alias) ;;
        *) continue ;;
    esac
    checked=$((checked+1))
    if [[ -e "$ROOT/$path" ]]; then
        present=$((present+1))
        # An alias must resolve to a real target, not merely exist: on Windows
        # a symlink entry extracts as a stub and the target check is the only
        # thing that notices.
        if [[ "$category" == "alias" && -n "$target" ]]; then
            if [[ ! -e "$(dirname "$ROOT/$path")/$target" ]]; then
                bad "$path is an alias whose target $target is missing"
            fi
        fi
    else
        bad "missing $category: $path"
    fi
done < <(grep -v '^[[:space:]]*#' "$MANIFEST" | grep -v '^[[:space:]]*$')

if [[ "$checked" -eq 0 ]]; then
    bad "manifest yielded no rows -- parse problem, not an empty requirement"
else
    ok "$present/$checked manifest-required artifacts present"
fi

# ---------------------------------------------------------------------------
# 2. Absence.  The point of the v0.11.0 packaging work is that these are GONE;
#    a presence-only gate cannot express that.
# ---------------------------------------------------------------------------
echo "-- the Python packaging path must be gone --"
for gone in pkg.py sfo.py crypt.c build_pkgcrypt.py; do
    if [[ -e "$ROOT/bin/$gone" ]]; then
        bad "bin/$gone is still present"
    else
        ok "bin/$gone absent"
    fi
done

if [[ ! -f "$ROOT/setup.cmd" ]]; then
    bad "setup.cmd missing"
elif grep -qi 'python\|pkgcrypt' "$ROOT/setup.cmd"; then
    bad "setup.cmd still mentions python/pkgcrypt"
    grep -in 'python\|pkgcrypt' "$ROOT/setup.cmd" | sed 's/^/          /'
else
    ok "setup.cmd is python-free"
fi

# ---------------------------------------------------------------------------
# 3. Version stamps must AGREE.  README.txt is written at package time from
#    version.sh; VERSION and cell/sdk_version.h are copied from the installed
#    SDK and only change when build-sdk.sh runs.  Cut a zip after tagging
#    without re-running build-sdk and they disagree.  See failure (1).
# ---------------------------------------------------------------------------
echo "-- version stamps must agree --"
hdr="$ROOT/ppu/include/cell/sdk_version.h"
# All three are read through the SAME vX.Y.Z extraction. Reading one of them
# differently is how a dev cut ends up "disagreeing" with itself: VERSION holds
# version.sh verbatim ("v0.11.3+dirty"), README embeds the same string in a
# sentence, and comparing a raw read against an extracted one reports a version
# mismatch when the only difference is the suffix.
_ver_of() { grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | head -1; }
readme_v="$(head -1 "$ROOT/README.txt" 2>/dev/null | _ver_of)"
version_v="$(_ver_of < "$ROOT/VERSION" 2>/dev/null)"
hdr_v="$(grep -m1 'define PS3SDK_VERSION ' "$hdr" 2>/dev/null | _ver_of)"
printf "    README.txt    = %s\n    VERSION       = %s\n    sdk_version.h = %s\n" \
       "${readme_v:-<none>}" "${version_v:-<none>}" "${hdr_v:-<none>}"

if [[ -z "$readme_v" || -z "$version_v" || -z "$hdr_v" ]]; then
    bad "could not read all three version strings"
elif [[ "$readme_v" != "$version_v" || "$readme_v" != "$hdr_v" ]]; then
    bad "version strings DISAGREE -- re-run build-sdk.sh with the tag in place, then re-package"
elif [[ -n "$WANT" && "$readme_v" != "$WANT" ]]; then
    bad "version is $readme_v but this cut was meant to be $WANT"
else
    ok "all three agree${WANT:+ and match $WANT}"
fi

# ON_TAG is fatal only for a RELEASE cut.  A dev package is legitimately made
# off a tag, and a gate that refuses to run outside a release would simply be
# switched off -- which costs us the checks that matter every day.  Callers
# that are cutting a release pass --require-on-tag; packaging does so only
# when version.sh produced a clean vX.Y.Z with no +suffix.
on_tag="$(grep -m1 'ON_TAG' "$hdr" 2>/dev/null | grep -oE '[01][[:space:]]*$' | tr -d '[:space:]')"
if [[ "$on_tag" == "1" ]]; then
    ok "sdk_version.h ON_TAG=1 (cut was made on the tag)"
elif [[ "$REQUIRE_ON_TAG" -eq 1 ]]; then
    bad "sdk_version.h ON_TAG=${on_tag:-<none>} -- a release cut must be made with the tag in place"
else
    printf "  note    sdk_version.h ON_TAG=%s (dev cut; pass --require-on-tag for a release)\n" \
           "${on_tag:-<none>}"
fi

# ---------------------------------------------------------------------------
# 4. Symbols.  This is the only section that can catch failure (2): a phase
#    that did not re-run leaves correct-looking stamps on stale archives.
# ---------------------------------------------------------------------------
echo "-- claimed fixes must be in the binaries --"
NM="${PS3_NM:-}"
[[ -n "$NM" ]] || NM="$(command -v powerpc64-ps3-elf-nm 2>/dev/null || true)"
if [[ -z "$NM" && "${OS:-}" == "Windows_NT" && -x "$ROOT/ppu/bin/powerpc64-ps3-elf-nm.exe" ]]; then
    # The tree ships a Windows nm.  Usable when the gate runs on Windows;
    # it is a PE, so it is deliberately NOT used from Linux/CI.
    NM="$ROOT/ppu/bin/powerpc64-ps3-elf-nm.exe"
fi

if [[ -z "$NM" ]]; then
    # Deliberately fatal.  A gate that skips its own symbol checks when the
    # tool is absent reports success for the exact case it exists to catch.
    bad "no cross nm found (set PS3_NM) -- refusing to skip the symbol checks"
else
    if [[ "${#REQUIRED_SYMBOLS[@]}" -eq 0 ]]; then
        bad "REQUIRED_SYMBOLS table is empty -- nothing would be verified"
    fi
    for row in "${REQUIRED_SYMBOLS[@]}"; do
        rel="${row%%|*}"
        rest="${row#*|}"
        syms="${rest%%|*}"
        mode="${rest##*|}"

        if [[ ! -f "$ROOT/$rel" ]]; then
            bad "$rel missing"
            continue
        fi

        if [[ "$mode" == "defined" ]]; then
            listing="$("$NM" --defined-only "$ROOT/$rel" 2>/dev/null)"
        else
            listing="$("$NM" "$ROOT/$rel" 2>/dev/null)"
        fi

        missing=""
        for sym in $syms; do
            if [[ "$mode" == "defined" ]]; then
                grep -qE " (T|W|D) $sym\$" <<<"$listing" || missing="$missing $sym"
            else
                grep -q "$sym" <<<"$listing" || missing="$missing $sym"
            fi
        done

        if [[ -n "$missing" ]]; then
            bad "$rel is missing:$missing -- the phase that builds it has not re-run since the fix landed"
        else
            ok "$rel has ${syms// /, }"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 4b. crt objects.  crtend.o's .eh_frame must be exactly the 4-byte zero
#     terminator (t_376721dc): a toolchain built without patch 0035 ships a
#     crtend carrying an FDE after the terminator, and GNU ld then emits
#     "no .eh_frame_hdr table will be created" on EVERY user link, which
#     breaks stderr-sensitive configure probes (cairo's pthread tier).
#     nm cannot see section sizes; readelf can, and any binutils readelf
#     reads a foreign ELF, so a host readelf is an acceptable fallback.
# ---------------------------------------------------------------------------
echo "-- crtend.o .eh_frame must be only the terminator --"
RD="${PS3_READELF:-}"
[[ -n "$RD" ]] || RD="$(command -v powerpc64-ps3-elf-readelf 2>/dev/null || true)"
if [[ -z "$RD" && "${OS:-}" == "Windows_NT" && -x "$ROOT/ppu/bin/powerpc64-ps3-elf-readelf.exe" ]]; then
    RD="$ROOT/ppu/bin/powerpc64-ps3-elf-readelf.exe"
fi
[[ -n "$RD" ]] || RD="$(command -v readelf 2>/dev/null || true)"
if [[ -z "$RD" ]]; then
    # Fatal for the same reason as the nm case above.
    bad "no readelf found (set PS3_READELF) -- refusing to skip the crtend check"
else
    crtends=("$ROOT"/ppu/lib/gcc/powerpc64-ps3-elf/*/crtend.o
             "$ROOT"/ppu/lib/gcc/powerpc64-ps3-elf/*/lp64/crtend.o)
    seen=0
    for f in "${crtends[@]}"; do
        [[ -f "$f" ]] || continue
        seen=$((seen+1))
        rel="${f#"$ROOT"/}"
        sz="$("$RD" -SW "$f" 2>/dev/null | sed 's/\[ *[0-9]*\]//' | awk '$1==".eh_frame"{print $5}')"
        if [[ -z "$sz" ]]; then
            bad "$rel: could not read .eh_frame section size"
        elif [[ "$sz" =~ ^0*4$ ]]; then
            ok "$rel .eh_frame is the 4-byte terminator only"
        else
            bad "$rel .eh_frame is 0x$sz -- an FDE after the terminator makes every link noisy (t_376721dc)"
        fi
    done
    if [[ "$seen" -lt 2 ]]; then
        # Both multilibs ship a crtend; matching fewer means the glob went
        # stale, and a check that finds nothing must not look clean.
        bad "found $seen crtend.o (expected 2: default + lp64 multilib)"
    fi
fi

# ---------------------------------------------------------------------------
# 5. Symlinks.  package-windows-release.sh already asserts this on the stage
#    tree and on the zip; this is the post-extract view, which is the one the
#    user actually gets.
# ---------------------------------------------------------------------------
echo "-- no symlinks (a Windows tree must not depend on them) --"
symcount="$(find "$ROOT" -type l 2>/dev/null | wc -l | tr -d '[:space:]')"
if [[ "$symcount" == "0" ]]; then
    ok "0 symlinks"
else
    bad "$symcount symlink(s) present -- they will not survive a Windows extract"
    find "$ROOT" -type l 2>/dev/null | head -10 | sed 's/^/          /'
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "RELEASE TREE OK"
else
    echo "RELEASE TREE REJECTED: $fail problem(s)"
fi
exit $fail

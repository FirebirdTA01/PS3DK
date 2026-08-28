#!/usr/bin/env bash
#
# gen-required-artifacts.sh — derive the sample-linked rows of
# cmake/ps3-required-artifacts.txt from the bundled samples themselves.
#
# WHY THIS EXISTS: the required-artifact list began as a hand-maintained array
# in package-windows-release.sh.  It held 26 entries while the samples we ship
# linked 95 libraries, so a release could omit librt.a / libgcm_cmd.a /
# libspurs.a and still validate clean.  A frozen list goes stale the first time
# a sample links something new, so the sample-linked rows are DERIVED and CI
# fails when the committed manifest stops covering them.
#
# Emits ppu_stub and spu_lib rows only.  host_bin / host_file / sdk_core /
# alias / optional rows are hand-maintained: they encode intent rather than
# linkage, and --check deliberately tolerates them.
#
# Usage:
#   gen-required-artifacts.sh <install-root>            # print derived rows
#   gen-required-artifacts.sh <install-root> --check    # verify coverage
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="${1:-${PS3DK:-}}"
MODE="${2:-emit}"
MANIFEST="$ROOT/cmake/ps3-required-artifacts.txt"

[[ -n "$INSTALL" && -d "$INSTALL" ]] || {
    echo "usage: $(basename "$0") <install-root> [--check]   (or set PS3DK)" >&2
    exit 2
}

# Every token inside a target_link_libraries(...) call across the bundled
# samples.  Tokens may be bare (rsx) or literal linker flags (-lspurs appears in
# samples/PSL1GHT/spu/spurs), so strip a leading -l.  The target name is swept
# up too; it is filtered out by requiring lib<name>.a to actually exist.
# PPU side: every token inside a target_link_libraries(...) call.  Tokens may be
# bare (rsx) or literal linker flags (-lspurs appears in samples/PSL1GHT/spu/
# spurs), so strip a leading -l.  The target name is swept up too; it is
# filtered out by requiring lib<name>.a to actually exist.
ppu_names="$(
    find "$ROOT/samples" -name CMakeLists.txt -print0 |
    xargs -0 awk '
        /target_link_libraries[[:space:]]*\(/ { inblk = 1 }
        inblk                                 { print }
        inblk && /\)/                         { inblk = 0 }
    ' |
    tr '(),' '   ' | tr -s '[:space:]' '
' | sed 's/^-l//' |
    grep -Ev '^(target_link_libraries|PRIVATE|PUBLIC|INTERFACE)$' |
    grep -Ev '^\$|^$' | sort -u
)"

# SPU side: ps3_add_spu_image(target NAME <n> SOURCES <files> [LIBS <libs...>]
# [CFLAGS ...] [JOBBIN_WRAP]).  SPU archives are named ONLY here, never in
# target_link_libraries, so scanning just the PPU link lines misses every SPU
# library — the manifest had zero SPU coverage for exactly that reason.
spu_names="$(
    find "$ROOT/samples" -name CMakeLists.txt -print0 |
    xargs -0 awk '
        /ps3_add_spu_image[[:space:]]*\(/ { inblk = 1; want = 0 }
        inblk {
            line = $0
            gsub(/[(),]/, " ", line)
            n = split(line, tok, /[[:space:]]+/)
            for (i = 1; i <= n; i++) {
                t = tok[i]
                if (t == "LIBS") { want = 1; continue }
                if (t == "CFLAGS" || t == "NAME" || t == "SOURCES" || t == "JOBBIN_WRAP") { want = 0; continue }
                if (want && t != "") print t
            }
        }
        inblk && /\)/ { inblk = 0; want = 0 }
    ' |
    sed 's/^-l//' | grep -Ev '^\$|^$' | sort -u
)"

# Stub aliases (libsysutil.a -> libsysutil_stub.a etc.) are asserted by the
# manifest's alias rows, which also check byte-identity with the target.
# Emitting them here would duplicate that with a weaker existence-only check.
alias_libs="$(awk '$1 == "alias" { n = split($2, p, "/"); print p[n] }' "$MANIFEST" | sort -u)"

emit() {
    local subdir="$1" category="$2" n lib
    while read -r n; do
        [[ -n "$n" ]] || continue
        lib="lib${n}.a"
        # Explicit ifs, not "test && cmd": under set -e a false test is a
        # failing statement and silently kills the loop (it produced an empty
        # manifest once already).
        if grep -qxF "$lib" <<< "$alias_libs"; then
            continue
        fi
        if [[ -f "$INSTALL/$subdir/$lib" ]]; then
            printf '%-9s %s/%s
' "$category" "$subdir" "$lib"
        fi
    done <<< "$3" | sort -u
}

derived="$( { emit ppu/lib ppu_stub "$ppu_names"; emit spu/lib spu_lib "$spu_names"; } )"
norm() { tr -s ' ' ' ' | sed 's/[[:space:]]*$//' | sort -u; }

if [[ "$MODE" == "--check" ]]; then
    # SUBSET, not equality: every sample-linked archive must be promised by the
    # manifest, but the manifest may promise more.  liblv2_stub.a is pulled in
    # by the link spec rather than any target_link_libraries line, and an
    # equality check would fail on it and pressure someone into deleting a real
    # requirement to go green.
    committed="$(grep -E '^(ppu_stub|spu_lib)[[:space:]]' "$MANIFEST" | norm)"
    normalized="$(printf '%s\n' "$derived" | norm)"
    # FLOOR: an empty derivation makes "missing" empty and the check pass with
    # "covers all 0".  That is the one failure mode this design cannot otherwise
    # see -- and it already happened once, when set -e plus a false "test && cmd"
    # silently killed the emit loop.  Both scanners are non-empty by construction
    # of this repo, so zero from either means the scanner broke, not that the
    # samples stopped linking.  (No "local" here: this block is script top level.)
    _np="$(printf '%s
' "$derived" | grep -c '^ppu_stub' || true)"
    _ns="$(printf '%s
' "$derived" | grep -c '^spu_lib' || true)"
    if [[ "$_np" -eq 0 ]]; then
        echo "gen-required-artifacts: PPU scanner produced 0 rows — the scanner is broken, not the samples." >&2
        exit 1
    fi
    if [[ "$_ns" -eq 0 ]]; then
        echo "gen-required-artifacts: SPU scanner produced 0 rows (ps3_add_spu_image LIBS) — the scanner is broken, not the samples." >&2
        exit 1
    fi

    missing="$(comm -23 <(printf '%s\n' "$normalized") <(printf '%s\n' "$committed") || true)"
    if [[ -z "$missing" ]]; then
        echo "gen-required-artifacts: manifest covers all $(printf '%s\n' "$normalized" | grep -c .) sample-linked archives (ppu_stub=$_np spu_lib=$_ns)"
        exit 0
    fi
    echo "gen-required-artifacts: MANIFEST INCOMPLETE — bundled samples link these, the manifest does not require them:" >&2
    printf '%s\n' "$missing" >&2
    echo "Regenerate with: scripts/gen-required-artifacts.sh <install-root>" >&2
    exit 1
fi

printf '%s\n' "$derived"

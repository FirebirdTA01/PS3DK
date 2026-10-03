#!/usr/bin/env bash
# installed-copy-map consistency test (t_cab4a49d).  Host-only: no compiler,
# no PSL1GHT clone, no build tree.  Cross-checks the three places that must
# agree about "which installed artifact replaces a vendored PSL1GHT copy":
#
#   1. scripts/installed-copy-map.tsv   (the canonical manifest)
#   2. the per-site log call sites       (REPLACING ... lines / ps3tc_replacing ...)
#   3. docs/installed-copy-map.md       (the table)
#   4. scripts/which-copy.sh --list     (the lookup table derived from #1)
#
# A regression that drifts one of the four is a silent "the build log greps to
# a list that is NOT the docs page" failure, which is exactly what this card
# is about ("a full build log greps to the complete list; the docs page lists
# the same set").  We can't run the full toolchain build here (no C toolchain),
# so instead of grepping a live build log we grep the *call sites* that would
# emit the log line, and assert the emitted strings are mutually consistent
# and complete.
#
# exit 0 = all checks pass, non-zero = first failing check is reported.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
MAP="$ROOT/scripts/installed-copy-map.tsv"
DOCS="$ROOT/docs/installed-copy-map.md"
WHICH="$ROOT/scripts/which-copy.sh"

fail=0
pass() { printf '  [PASS] %s\n' "$1"; }
errj() { printf '  [FAIL] %s\n' "$1"; fail=1; }

# ---------------------------------------------------------------- manifest --
echo "== 1. manifest well-formed =="
if [[ ! -f "$MAP" ]]; then errj "manifest missing: $MAP"; exit 1; fi
map_rows=()
nrow=0
while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" ]] && continue
    cols="$(awk -F'\t' '{print NF}' <<<"$line")"
    if [[ "$cols" != "3" ]]; then
        errj "row has $cols columns (want 3): $(printf '%q' "$line")"
        continue
    fi
    map_rows+=("$line")
    nrow=$((nrow+1))
done < "$MAP"
[[ "$nrow" -ge 1 ]] && pass "manifest parsed: $nrow rows, 3 columns each" \
                 || errj "manifest has no data rows"

# installed-path set + source set from the manifest
manifest_paths() { awk -F'\t' 'NF==3{print $1}' "$MAP"; }
manifest_sources() { awk -F'\t' 'NF==3{print $2}' "$MAP"; }

# ------------------------------------------------------------ source exists --
echo "== 2. every source dir exists on disk =="
while IFS= read -r src; do
    if [[ -e "$ROOT/$src" ]]; then
        pass "$src"
    else
        errj "source not found: $src"
    fi
done < <(manifest_sources | sort -u)

# ---------------------------------------------------------- logged at a site --
echo "== 3. each manifest row is logged at its builder's call site =="
# A site may log one of two equivalent ways:
#   Makefile:   @echo "REPLACING <path> with <src> (vendored copy ...)"
#   shell:      ps3tc_replacing "<path>" "<src>"
while IFS=$'\t' read -r p s b; do
    [[ -n "$p" ]] || continue
    builder="$ROOT/$b"
    if [[ ! -f "$builder" ]]; then
        errj "builder file missing: $b (for $p)"
        continue
    fi
    if grep -qF "REPLACING ${p} with ${s} " "$builder" \
       || grep -qF "ps3tc_replacing \"${p}\" \"${s}\"" "$builder"; then
        pass "$b  ->  $p  (src $s)"
    else
        errj "no REPLACING log site found in $b for '$p' (src '$s')"
    fi
done < <(awk -F'\t' 'NF==3' "$MAP")
:

# ----------------------------------- reverse: no stray logged paths not mapped --
echo "== 4. every installed path logged anywhere is present in the manifest =="
# Collect installed paths as they appear at log call sites (both forms) across
# the whole tree, then confirm each is a manifest row.
logged_paths() {
    {
        grep -rhoE 'REPLACING (ppu/[A-Za-z0-9_./-]+) with ' "$ROOT"/sdk/*/Makefile "$ROOT"/sdk/Makefile 2>/dev/null \
            | sed -E 's/^REPLACING //; s/ with $//'
        grep -rhoE 'ps3tc_replacing "(ppu/[A-Za-z0-9_./-]+)"' "$ROOT"/scripts/*.sh 2>/dev/null \
            | sed -E 's/^ps3tc_replacing "//; s/"$//'
    } | sort -u
}
while IFS= read -r lp; do
    [[ -n "$lp" ]] || continue
    if manifest_paths | grep -qFx "$lp"; then
        pass "logged path is mapped:  $lp"
    else
        errj "logged path NOT in manifest:  $lp"
    fi
done < <(logged_paths)

# ------------------------------------------------------- docs lists same set --
echo "== 5. docs table lists exactly the manifest installed-path set =="
if [[ ! -f "$DOCS" ]]; then
    errj "docs page missing: $DOCS"
else
    docs_paths() {
        grep -oE '`ppu/[A-Za-z0-9_./-]+\.(a|h)`' "$DOCS" \
            | sed -E 's/^`//; s/`$//' | sort -u
    }
    want="$(manifest_paths | sort -u)"
    got="$(docs_paths)"
    if [[ "$want" == "$got" ]]; then
        pass "docs installed-path set == manifest set ($(wc -l <<<"$want") paths)"
    else
        errj "docs set != manifest set"
        printf '    only-in-manifest:\n';  comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/      /'
        printf '    only-in-docs:\n';     comm -13 <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/      /'
    fi
fi

# -------------------------------------------------- which-copy --list matches --
echo "== 6. which-copy.sh --list returns exactly the manifest set =="
if [[ ! -f "$WHICH" ]]; then
    errj "which-copy.sh missing: $WHICH"
else
    list_out="$(bash "$WHICH" --list 2>/dev/null || true)"
    which_paths="$(grep -oE 'ppu/[A-Za-z0-9_./-]+\.(a|h)' <<<"$list_out" | sort -u)"
    want="$(manifest_paths | sort -u)"
    if [[ "$want" == "$which_paths" ]]; then
        pass "which-copy --list set == manifest set"
    else
        errj "which-copy --list set != manifest set"
        printf '    want:\n'; printf '%s\n' "$want" | sed 's/^/      /'
        printf '    got : \n'; printf '%s\n' "$which_paths" | sed 's/^/      /'
    fi
fi

echo
if [[ "$fail" -eq 0 ]]; then
    echo "installed-copy-map consistency: ALL CHECKS PASS"
else
    echo "installed-copy-map consistency: FAILURES DETECTED"
fi
exit "$fail"

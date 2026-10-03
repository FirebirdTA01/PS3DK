#!/usr/bin/env bash
# which-copy.sh (t_cab4a49d) -- for an installed SDK path, report where OUR copy
# came from and whether it replaces a vendored PSL1GHT one.  Answers "which
# builder makes this artifact?" backed by scripts/installed-copy-map.tsv.
#
# usage:
#   which-copy.sh <installed-path>   print source + builder + status for one artifact
#   which-copy.sh --list             dump the full installed-copy map
#   which-copy.sh --help
#
# <installed-path> may be repo-relative (ppu/lib/librsx.a), an install-prefix
# path ($PS3DK/ppu/lib/librsx.a), or a bare filename (librsx.a).
# exit: 0 found, 1 not in the map, 2 usage error.

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
map="$script_dir/installed-copy-map.tsv"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,12p' "$0"
    exit 0
fi

if [[ ! -f "$map" ]]; then
    echo "which-copy: manifest missing: $map" >&2
    exit 2
fi

if [[ "${1:-}" == "--list" ]]; then
    printf '%-34s  %-22s  %s\n' "INSTALLED" "SRC(OUR)" "BUILDER"
    printf '%-34s  %-22s  %s\n' "-------" "--------" "-------"
    while IFS=$'\t' read -r p s b; do
        [[ -n "$p" ]] || continue
        printf '%-34s  %-22s  %s\n' "$p" "$s" "$b"
    done < "$map"
    exit 0
fi

if [[ $# -lt 1 ]]; then
    echo "which-copy: no path given (try --list or --help)" >&2
    exit 2
fi

# Normalise: backslashes->slashes, and an absolute .../ppu/<rest> to <rest>
# prefixed with ppu/.
arg="${1//\\//}"
if [[ "$arg" == */ppu/* ]]; then
    arg="ppu/${arg##*/ppu/}"
fi
base="${arg##*/}"

# Prefer a full-path row; else fall back to a basename row (handles bare
# "librt.a" or a bare "gcm_sys.h").
row="$(awk -F'\t' -v p="$arg" '$1==p {print; exit}' "$map")"
if [[ -z "$row" ]]; then
    row="$(awk -F'\t' -v b="$base" '{n=split($1,a,"/"); if (a[n]==b) {print; exit}}' "$map")"
fi

if [[ -z "$row" ]]; then
    echo "which-copy: '$1' is not a known PSL1GHT-replacing artifact (checked $map)" >&2
    echo "  try: which-copy.sh --list" >&2
    exit 1
fi

p="${row%%$'\t'*}"
rest="${row#*$'\t'}"
s="${rest%%$'\t'*}"
b="${rest#*$'\t'}"

echo "installed path : $arg"
echo "source (ours)  : $s"
echo "builder        : $b"
echo "status         : REPLACING $p with $s (vendored PSL1GHT copy at $p is NOT installed)"

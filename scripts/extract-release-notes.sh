#!/usr/bin/env bash
# Extract the release notes body for a version from CHANGELOG.md.

set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 VERSION [CHANGELOG] [OUTPUT]

Extracts the matching CHANGELOG section for VERSION. VERSION may be either
vX.Y.Z or X.Y.Z; both heading forms are accepted. If no matching version
heading exists, falls back to a non-empty [Unreleased] section so a tag push
does not publish an empty generic release when the changelog was updated but
not promoted.
EOF
}

die() {
    printf "extract-release-notes: ERROR: %s\n" "$*" >&2
    exit 1
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

version="${1:-}"
changelog="${2:-CHANGELOG.md}"
output="${3:-}"

[[ -n "$version" ]] || { usage >&2; exit 2; }
[[ -f "$changelog" ]] || die "changelog not found: $changelog"

tmp="$(mktemp)"
cleaned="$(mktemp)"
trap 'rm -f "$tmp" "$cleaned"' EXIT

clean_section() {
    awk '
        /<!--/ { in_comment = 1; next }
        in_comment && /-->/ { in_comment = 0; next }
        in_comment { next }
        { lines[++n] = $0 }
        END {
            start = 1
            while (start <= n && lines[start] ~ /^[[:space:]]*$/) {
                start++
            }
            end = n
            while (end >= start && lines[end] ~ /^[[:space:]]*$/) {
                end--
            }
            for (i = start; i <= end; i++) {
                print lines[i]
            }
        }
    ' "$1"
}

extract_section() {
    local heading="$1"
    awk -v want="$heading" '
        $0 ~ "^## \\[" want "\\]([[:space:]]|$)" { capture = 1; found = 1; next }
        capture && /^## \[/ { exit }
        capture { print }
        END { if (!found) exit 2 }
    ' "$changelog"
}

write_output() {
    if [[ -n "$output" ]]; then
        cp "$cleaned" "$output"
    else
        cat "$cleaned"
    fi
}

bare="${version#v}"
candidates=("$version")
if [[ "$bare" != "$version" ]]; then
    candidates+=("$bare")
else
    candidates+=("v$version")
fi

for candidate in "${candidates[@]}"; do
    if extract_section "$candidate" >"$tmp"; then
        clean_section "$tmp" >"$cleaned"
        if [[ -s "$cleaned" ]]; then
            write_output
            exit 0
        fi
    fi
done

if extract_section "Unreleased" >"$tmp"; then
    clean_section "$tmp" >"$cleaned"
    if [[ -s "$cleaned" ]]; then
        printf "extract-release-notes: WARNING: no CHANGELOG section for %s; using [Unreleased]\n" "$version" >&2
        write_output
        exit 0
    fi
fi

die "no non-empty CHANGELOG section found for $version or [Unreleased]"

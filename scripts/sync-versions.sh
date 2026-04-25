#!/usr/bin/env bash
# PS3 Custom Toolchain — sync version into build files that can't read
# scripts/version.sh at build time (i.e. tools/Cargo.toml).
#
# Cargo refuses to read its own manifest from a generator at build time,
# so we instead rewrite tools/Cargo.toml's [workspace.package] version
# field idempotently from scripts/version.sh.  Run this:
#   - Before tagging a release.
#   - In CI on tag-push (the release workflow does this automatically).
#   - Whenever you need the Rust tools to report the current version
#     via `--version` (read at compile time from CARGO_PKG_VERSION).
#
# The script is idempotent: re-running with the same git state is a
# no-op.  On change it prints a one-line summary; otherwise silent.
#
# Usage:
#   scripts/sync-versions.sh           # write
#   scripts/sync-versions.sh --check   # exit 1 if Cargo.toml is out of sync
#   scripts/sync-versions.sh --dry-run # print what would change

set -euo pipefail

mode="write"
for arg in "$@"; do
    case "$arg" in
        --check)   mode="check" ;;
        --dry-run) mode="dry-run" ;;
        -h|--help)
            sed -n '2,18p' "$0"
            exit 0
            ;;
        *)
            echo "sync-versions.sh: unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

bare_version="$(scripts/version.sh --format=bare)"
cargo_toml="tools/Cargo.toml"

if [ ! -f "$cargo_toml" ]; then
    echo "sync-versions.sh: $cargo_toml not found" >&2
    exit 1
fi

# Match the version line inside [workspace.package].  We deliberately use
# a narrow regex anchored to the workspace.package block by walking with
# awk rather than a global sed, so we don't accidentally rewrite some
# other version string that happens to live in the file.
current="$(awk '
    /^\[workspace\.package\]/ { in_block = 1; next }
    /^\[/ && in_block        { in_block = 0 }
    in_block && /^version[[:space:]]*=/ {
        # Strip key, =, quotes, comments.
        sub(/^version[[:space:]]*=[[:space:]]*"/, "")
        sub(/".*$/, "")
        print
        exit
    }
' "$cargo_toml")"

if [ -z "$current" ]; then
    echo "sync-versions.sh: could not find [workspace.package] version in $cargo_toml" >&2
    exit 1
fi

if [ "$current" = "$bare_version" ]; then
    case "$mode" in
        check|dry-run|write) exit 0 ;;
    esac
fi

case "$mode" in
    check)
        echo "sync-versions.sh: $cargo_toml workspace.package.version is '$current', expected '$bare_version'" >&2
        echo "sync-versions.sh: run 'scripts/sync-versions.sh' to fix" >&2
        exit 1
        ;;
    dry-run)
        echo "sync-versions.sh: would rewrite $cargo_toml workspace.package.version: $current -> $bare_version"
        exit 0
        ;;
    write)
        # In-place rewrite scoped to the [workspace.package] block.
        tmp="$(mktemp)"
        awk -v want="$bare_version" '
            /^\[workspace\.package\]/ { in_block = 1; print; next }
            /^\[/ && in_block        { in_block = 0 }
            in_block && /^version[[:space:]]*=/ {
                print "version = \"" want "\""
                next
            }
            { print }
        ' "$cargo_toml" > "$tmp"
        mv "$tmp" "$cargo_toml"
        echo "sync-versions.sh: $cargo_toml workspace.package.version: $current -> $bare_version"
        ;;
esac

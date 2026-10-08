#!/usr/bin/env bash
# PS3 Custom Toolchain — sync the release version into tools/VERSION, the
# one build input that cannot read scripts/version.sh at build time.
#
# Every Rust tool prints tools/VERSION for --version (crate ps3dk-version
# embeds it with include_str!).  The crate version in tools/Cargo.toml stays
# fixed at 0.0.0: Cargo hashes the crate version into every symbol, so the
# per-release bump this script used to make there changed the code layout of
# every tool, not only its version string.  Run this:
#   - Before tagging a release.
#   - In CI on tag-push (the release workflow does this automatically).
#   - Whenever you need the Rust tools to report the current version.
#
# The script is idempotent: re-running with the same git state is a
# no-op.  On change it prints a one-line summary; otherwise silent.
#
# Usage:
#   scripts/sync-versions.sh           # write
#   scripts/sync-versions.sh --check   # exit 1 if tools/VERSION is out of sync
#   scripts/sync-versions.sh --dry-run # print what would change
#   scripts/sync-versions.sh --version=0.13.0   # stamp an explicit version
#
# --version exists because version.sh derives the number from the tag, and at
# release time the stamp has to land in the commit the tag will point AT - the
# tag does not exist yet.

set -euo pipefail

mode="write"
override=""
for arg in "$@"; do
    case "$arg" in
        --check)   mode="check" ;;
        --dry-run) mode="dry-run" ;;
        --version=*)
            override="${arg#--version=}"
            # A number typed by hand is the entire point of this flag, so
            # nothing downstream can catch a typo in it: a malformed value
            # would be stamped into tools/VERSION verbatim.  Reject it here.
            if ! printf '%s' "$override" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
                echo "sync-versions.sh: --version must be X.Y.Z (got '$override')" >&2
                exit 1
            fi
            ;;
        -h|--help)
            sed -n '2,24p' "$0"
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

if [ -n "$override" ]; then
    bare_version="$override"
else
    bare_version="$(scripts/version.sh --format=bare)"
fi
version_file="tools/VERSION"

if [ ! -f "$version_file" ]; then
    echo "sync-versions.sh: $version_file not found" >&2
    exit 1
fi
# The file is in sync only when it is exactly "X.Y.Z" and a newline.  Compare
# bytes, not a whitespace-stripped reading: " 0.20.6" or "0.20\n.6" would
# strip to the right number here, while the Rust side (which trims only
# trailing whitespace) would embed something else.  Write mode rewrites any
# other content to the canonical form.
if printf '%s\n' "$bare_version" | cmp -s - "$version_file"; then
    exit 0
fi
# For messages only: the start of the file as sed -n l shows it, each line
# end as $, so a stray space or an extra line is visible.
current="$(head -c 64 "$version_file" | sed -n l | tr '\n' ' ' | sed 's/ $//')"

case "$mode" in
    check)
        echo "sync-versions.sh: $version_file is '$current', expected '$bare_version'" >&2
        echo "sync-versions.sh: run 'scripts/sync-versions.sh' to fix" >&2
        exit 1
        ;;
    dry-run)
        echo "sync-versions.sh: would rewrite $version_file: $current -> $bare_version"
        ;;
    write)
        printf '%s\n' "$bare_version" > "$version_file"
        echo "sync-versions.sh: $version_file: $current -> $bare_version"
        ;;
esac

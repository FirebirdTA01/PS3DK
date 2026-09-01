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
#   scripts/sync-versions.sh --check   # exit 1 if the workspace is out of sync
#   scripts/sync-versions.sh --dry-run # print what would change
#   scripts/sync-versions.sh --version=0.13.0   # stamp an explicit version
#
# --version exists because version.sh derives the number from the tag, and at
# release time the stamp has to land in the commit the tag will point AT - the
# tag does not exist yet.  Both files were hand-edited at every cut until now.
#
# Both tools/Cargo.toml and tools/Cargo.lock are stamped.  The lock repeats the
# workspace version once per member, and leaving it stale breaks
# `cargo build --locked`.

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
            # would be stamped into both files verbatim.  Reject it here.
            if ! printf '%s' "$override" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
                echo "sync-versions.sh: --version must be X.Y.Z (got '$override')" >&2
                exit 1
            fi
            ;;
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

if [ -n "$override" ]; then
    bare_version="$override"
else
    bare_version="$(scripts/version.sh --format=bare)"
fi
cargo_toml="tools/Cargo.toml"
cargo_lock="tools/Cargo.lock"

# Cargo.lock carries a [[package]] entry per workspace member, each repeating
# the workspace version, so stamping Cargo.toml alone leaves the lock stale and
# `cargo build --locked` refuses.
#
# Members are read from Cargo.toml's [workspace] members list and matched by
# NAME - never by "entries that look like our version".  Third-party crates sit
# at versions that can coincide with ours: at this cut the lock held
# linux-raw-sys 0.12.1 and parking_lot 0.12.5 beside six of ours at 0.12.46,
# and a version-shaped rewrite would have pinned both to a release that does
# not exist.  The list is re-derived every run because it changes - parking_lot
# was not in the lock one cut earlier.
sync_lock() {
    local mode="$1" want="$2"
    [ -f "$cargo_lock" ] || return 0

    # [workspace] members lists DIRECTORIES.  Cargo.lock keys on PACKAGE
    # names.  They are identical for all six members today, but nothing
    # enforces that, and guessing the package name from the directory would
    # fail SILENTLY for a member where they differ: the entry simply would not
    # match, nothing would be stamped, cmp would see no difference, --check
    # would PASS, and the break would surface only as `cargo --locked`
    # refusing in the release build - loud, but late and far from the cause.
    # So read each member's own [package] name, which is authoritative.
    local dirs member_dir member_name members
    dirs="$(awk '
        /^members[[:space:]]*=/ { in_list = 1 }
        in_list {
            line = $0
            while (match(line, /"[^"]+"/)) {
                print substr(line, RSTART + 1, RLENGTH - 2)
                line = substr(line, RSTART + RLENGTH)
            }
            if (line ~ /\]/) in_list = 0
        }
    ' "$cargo_toml")"
    if [ -z "$dirs" ]; then
        echo "sync-versions.sh: no [workspace] members found in $cargo_toml" >&2
        exit 1
    fi

    members=""
    local expected=0
    for member_dir in $dirs; do
        local member_toml="$(dirname "$cargo_toml")/$member_dir/Cargo.toml"
        if [ ! -f "$member_toml" ]; then
            echo "sync-versions.sh: workspace member '$member_dir' has no Cargo.toml at $member_toml" >&2
            exit 1
        fi
        member_name="$(awk '
            /^\[package\]/ { in_pkg = 1; next }
            /^\[/           { in_pkg = 0 }
            in_pkg && /^name[[:space:]]*=/ {
                sub(/^name[[:space:]]*=[[:space:]]*"/, "")
                sub(/".*$/, "")
                print
                exit
            }
        ' "$member_toml")"
        if [ -z "$member_name" ]; then
            echo "sync-versions.sh: could not read [package] name from $member_toml" >&2
            exit 1
        fi
        members="$members $member_name"
        expected=$((expected + 1))
    done

    # And prove every one of them was actually FOUND in the lock.  Without
    # this, a member missing from the lock is indistinguishable from a member
    # already at the right version: both leave the file unchanged.
    local found
    found="$(awk -v members="$members" '
        BEGIN {
            n = split(members, m, " ")
            for (i = 1; i <= n; i++) if (m[i] != "") is_member[m[i]] = 1
        }
        /^\[\[package\]\]/ { name = "" }
        /^name = "/ {
            name = $0; sub(/^name = "/, "", name); sub(/".*$/, "", name)
            if (name in is_member) seen[name] = 1
        }
        END { c = 0; for (k in seen) c++; print c }
    ' "$cargo_lock")"
    if [ "$found" -ne "$expected" ]; then
        echo "sync-versions.sh: $cargo_lock has $found of $expected workspace members" >&2
        echo "sync-versions.sh: refusing rather than silently leaving members unstamped" >&2
        exit 1
    fi

    local tmp
    tmp="$(mktemp)"
    awk -v want="$want" -v members="$members" '
        BEGIN {
            n = split(members, m, " ")
            for (i = 1; i <= n; i++) if (m[i] != "") is_member[m[i]] = 1
        }
        /^\[\[package\]\]/ { name = "" }
        /^name = "/ {
            name = $0; sub(/^name = "/, "", name); sub(/".*$/, "", name)
        }
        /^version = "/ && name != "" && (name in is_member) {
            $0 = "version = \"" want "\""
        }
        { print }
    ' "$cargo_lock" > "$tmp"

    if cmp -s "$tmp" "$cargo_lock"; then
        rm -f "$tmp"
        return 0
    fi

    case "$mode" in
        check)
            rm -f "$tmp"
            echo "sync-versions.sh: $cargo_lock workspace member versions are not '$want'" >&2
            echo "sync-versions.sh: run 'scripts/sync-versions.sh' to fix" >&2
            rc=1
            ;;
        dry-run)
            rm -f "$tmp"
            echo "sync-versions.sh: would rewrite $cargo_lock workspace member versions to $want"
            ;;
        write)
            mv "$tmp" "$cargo_lock"
            echo "sync-versions.sh: $cargo_lock workspace member versions -> $want"
            ;;
    esac
}

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

rc=0
if [ "$current" = "$bare_version" ]; then
    # Cargo.toml being correct says nothing about the lock: the two were
    # stamped by different hands until now, so check it before exiting.
    sync_lock "$mode" "$bare_version"
    exit "$rc"
fi

# Both files are reported before exiting.  An early `exit` here would have
# hidden the lock entirely in --dry-run, and made --check report one file per
# run - which is the same "tells you about half the problem" shape that made
# the lock get hand-stamped in the first place.
case "$mode" in
    check)
        echo "sync-versions.sh: $cargo_toml workspace.package.version is '$current', expected '$bare_version'" >&2
        echo "sync-versions.sh: run 'scripts/sync-versions.sh' to fix" >&2
        rc=1
        ;;
    dry-run)
        echo "sync-versions.sh: would rewrite $cargo_toml workspace.package.version: $current -> $bare_version"
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

sync_lock "$mode" "$bare_version"
exit "$rc"

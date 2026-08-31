#!/usr/bin/env bash
#
# list-rust-bins.sh — the single source of truth for which binaries the
# tools/ Rust workspace produces.
#
# WHY THIS EXISTS: the same list used to be hardcoded in three places —
# scripts/install-host-tools.sh, .github/workflows/release.yml and
# scripts/build-host-tools-windows.sh — and they drifted, as hardcoded
# duplicates do.  install-host-tools.sh named four of the seven, so a
# from-source Linux install silently lacked prx-gen and the SFO tools that the
# Windows release shipped, and the gap only surfaced at the first ps3_add_prx
# or ps3_add_pkg.  release.yml's linux tools zip was missing prx-gen and
# spu-elf-to-ppu-obj for the same reason.  The names are derivable from
# tools/Cargo.toml, so derive them.
#
# Usage:
#   list-rust-bins.sh                              # one binary name per line
#   list-rust-bins.sh --check <dir> [--suffix .exe]
#       Verify <dir> holds every derived binary (with an optional filename
#       suffix for Windows staging).  Exits non-zero naming what is absent.
#
# The check is deliberately one-directional: it asserts derived ⊆ shipped.
# The reverse would be noise, because every consumer's directory also holds
# tools this workspace does not build (pkg.exe, sprxlinker, rsx-cg-compiler,
# the PSL1GHT helpers).  What matters is that a NEW [[bin]] cannot be added
# and then quietly not shipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORKSPACE="$ROOT/tools/Cargo.toml"

die() { printf "list-rust-bins: ERROR: %s\n" "$*" >&2; exit 1; }

[[ -f "$WORKSPACE" ]] || die "workspace manifest not found: $WORKSPACE"

# Workspace members, from the `members = [ ... ]` array.  Read only that array:
# a bare `grep '"'` over the file would also collect dependency names, edition
# strings and the repository URL.
members="$(awk '
    /^[[:space:]]*members[[:space:]]*=/ { inside = 1 }
    inside {
        if (match($0, /\]/)) inside = 0
        while (match($0, /"[^"]+"/)) {
            name = substr($0, RSTART + 1, RLENGTH - 2)
            print name
            $0 = substr($0, RSTART + RLENGTH)
        }
    }
' "$WORKSPACE")"

[[ -n "$members" ]] || die "no workspace members parsed from $WORKSPACE"

bins=""
while read -r member; do
    [[ -n "$member" ]] || continue
    manifest="$ROOT/tools/$member/Cargo.toml"
    [[ -f "$manifest" ]] || die "member '$member' has no Cargo.toml at $manifest"

    # Each `name = "..."` that belongs to a [[bin]] table.  A crate with no
    # [[bin]] falls back to cargo's own rule: the binary takes the package
    # name when src/main.rs exists.
    member_bins="$(awk '
        /^[[:space:]]*\[\[bin\]\]/ { inbin = 1; next }
        /^[[:space:]]*\[/          { inbin = 0 }
        inbin && /^[[:space:]]*name[[:space:]]*=/ {
            if (match($0, /"[^"]+"/)) print substr($0, RSTART + 1, RLENGTH - 2)
        }
    ' "$manifest")"

    if [[ -z "$member_bins" ]]; then
        if [[ -f "$ROOT/tools/$member/src/main.rs" ]]; then
            # Cargo's implicit binary is named after the PACKAGE, not the
            # directory the package sits in -- `members = ["foo-dir"]` with
            # `name = "bar"` builds bar, not foo-dir.  Using the member name
            # here would make this script confidently wrong for exactly the
            # crate it exists to protect: cargo would emit one name while
            # every consumer checked for another.  Every member today
            # declares [[bin]] explicitly, so this path is the future guard,
            # which is the reason to get it right rather than the reason not
            # to bother.
            member_bins="$(awk '
                /^[[:space:]]*\[package\]/ { inpkg = 1; next }
                /^[[:space:]]*\[/           { inpkg = 0 }
                inpkg && /^[[:space:]]*name[[:space:]]*=/ {
                    if (match($0, /"[^"]+"/)) {
                        print substr($0, RSTART + 1, RLENGTH - 2)
                        exit
                    }
                }
            ' "$manifest")"
            [[ -n "$member_bins" ]]                 || die "member '$member' has src/main.rs but no [package] name in $manifest"
        else
            # Neither an explicit [[bin]] nor an implicit one.  Silently
            # producing nothing here is how a tool goes missing from every
            # consumer at once, which is the failure this script exists to
            # prevent -- so say so instead.
            die "member '$member' declares no [[bin]] and has no src/main.rs"
        fi
    fi
    bins="$bins$member_bins"$'\n'
done <<< "$members"

bins="$(printf '%s' "$bins" | sed '/^$/d' | sort -u)"
[[ -n "$bins" ]] || die "no binaries derived from $WORKSPACE"

mode="${1:-emit}"
case "$mode" in
    emit|"")
        printf '%s\n' "$bins"
        ;;
    --check)
        dir="${2:-}"
        [[ -n "$dir" ]] || die "--check needs a directory"
        [[ -d "$dir" ]] || die "--check directory does not exist: $dir"
        suffix=""
        if [[ "${3:-}" == "--suffix" ]]; then
            suffix="${4:-}"
        fi
        missing=""
        count=0
        while read -r bin; do
            count=$((count + 1))
            [[ -f "$dir/$bin$suffix" ]] || missing="$missing $bin$suffix"
        done <<< "$bins"
        if [[ -n "$missing" ]]; then
            printf 'list-rust-bins: %s does not hold every workspace binary.\n' "$dir" >&2
            printf 'list-rust-bins: missing:%s\n' "$missing" >&2
            printf 'list-rust-bins: a [[bin]] in tools/Cargo.toml is not being shipped.\n' >&2
            exit 1
        fi
        printf 'list-rust-bins: %s holds all %d workspace binaries\n' "$dir" "$count"
        ;;
    *)
        die "unknown argument: $mode (expected --check)"
        ;;
esac

#!/usr/bin/env bash
# Refuse to build the Rust host tools with a rustc older than the one
# tools/Cargo.toml declares (rust-version).
#
# Cargo itself does not get that far: it downloads and parses every locked
# dependency's manifest first, and with an old cargo the first one that uses
# a newer edition stops the build with "feature `edition2024` is required",
# which says nothing about upgrading Rust.  Ubuntu 24.04's packaged cargo
# 1.75 hit exactly that building the SDK from the release source tarball.
#
# Usage: scripts/check-rust-version.sh   (exit 0 when rustc is new enough)

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
manifest="$script_dir/../tools/Cargo.toml"

need="$(sed -n 's/^rust-version *= *"\([0-9.]*\)".*/\1/p' "$manifest" | head -n 1)"
[[ -n "$need" ]] || { echo "check-rust-version: no rust-version in $manifest" >&2; exit 2; }

if ! command -v rustc >/dev/null 2>&1; then
    echo "ERROR: rustc not found; the SDK's host tools need Rust $need or later." >&2
    echo "       Install it with rustup: https://rustup.rs" >&2
    exit 1
fi
have="$(rustc --version | awk '{print $2}')"

# Compare MAJOR.MINOR[.PATCH] numerically.
older() {
    local IFS=.
    local -a a=($1) b=($2)
    local i
    for i in 0 1 2; do
        local x=${a[i]:-0} y=${b[i]:-0}
        x=${x%%[!0-9]*}; y=${y%%[!0-9]*}
        (( 10#${x:-0} < 10#${y:-0} )) && return 0
        (( 10#${x:-0} > 10#${y:-0} )) && return 1
    done
    return 1
}

if older "$have" "$need"; then
    echo "ERROR: rustc $have ($(command -v rustc)) is too old: the SDK's host tools need Rust $need or later." >&2
    echo "       Distribution packages are often older (Ubuntu 24.04 ships 1.75)." >&2
    echo "       Install a current toolchain with rustup (https://rustup.rs) and put ~/.cargo/bin first on PATH." >&2
    exit 1
fi
echo "check-rust-version: rustc $have >= $need"

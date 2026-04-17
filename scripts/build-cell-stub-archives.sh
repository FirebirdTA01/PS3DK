#!/usr/bin/env bash
# PS3 Custom Toolchain — Phase 6.5: stub-only Sony libraries
#
# Builds self-contained `lib<name>_stub.a` archives for libraries that have no
# PSL1GHT runtime backing (i.e. cell/* headers ship as declaration-only and
# need a stub archive to satisfy the link).  Drives `nidgen archive` for each
# YAML in $STUB_YAMLS, then installs the resulting archive under
# $PS3DEV/psl1ght/ppu/lib/ so samples can link with `-l<archive_name>_stub`.
#
# Add a YAML to $STUB_YAMLS as new zero-coverage libraries land
# (libsysutil_ap, libsysutil_imejp, libsysutil_subdisplay, libsysutil_music*,
# etc.).
#
# Usage:
#   source ./scripts/env.sh
#   ./scripts/build-cell-stub-archives.sh

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[stub-archives] %s\n" "$*"; }
die() { printf "[stub-archives] ERROR: %s\n" "$*" >&2; exit 1; }

[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-as" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."
[[ -d "$PS3DEV/psl1ght/ppu/lib" ]] \
    || die "PSL1GHT not installed. Run scripts/build-psl1ght.sh first."

NIDGEN_BIN="$PS3_TOOLCHAIN_ROOT/tools/target/release/nidgen"
if [[ ! -x "$NIDGEN_BIN" ]]; then
    say "building nidgen (release)"
    cargo build --release --manifest-path "$PS3_TOOLCHAIN_ROOT/tools/nidgen/Cargo.toml"
fi

# Phase 6.5 stub-archive YAMLs.  One entry per zero-PSL1GHT-coverage library.
STUB_YAMLS=(
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_screenshot_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_ap_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysmodule_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libl10n_stub.yaml"
)

OUT_ROOT="$PS3_TOOLCHAIN_ROOT/build/stub-archives"
INSTALL_LIB="$PS3DEV/psl1ght/ppu/lib"

mkdir -p "$OUT_ROOT" "$INSTALL_LIB"

for yaml in "${STUB_YAMLS[@]}"; do
    [[ -f "$yaml" ]] || die "missing YAML: $yaml"
    name="$(basename "$yaml" .yaml)"
    out_dir="$OUT_ROOT/$name"
    mkdir -p "$out_dir"

    say "building $name"
    "$NIDGEN_BIN" archive \
        --input "$yaml" \
        --toolchain-bin "$PS3DEV/ppu/bin" \
        --out-dir "$out_dir"

    # Find the produced archive (basename comes from the YAML's archive_name
    # or library field — easier to glob than to re-derive).
    shopt -s nullglob
    produced=( "$out_dir"/lib*_stub.a )
    shopt -u nullglob
    [[ ${#produced[@]} -eq 1 ]] \
        || die "expected exactly one archive in $out_dir, got ${#produced[@]}"

    install -m 0644 "${produced[0]}" "$INSTALL_LIB/"
    say "installed $(basename "${produced[0]}") -> $INSTALL_LIB/"
done

say "done — ${#STUB_YAMLS[@]} stub archive(s) installed"

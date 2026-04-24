#!/usr/bin/env bash
# PS3 Custom Toolchain — stub-only cell-SDK libraries
#
# Builds self-contained `lib<name>_stub.a` archives for libraries that have no
# PSL1GHT runtime backing (i.e. cell/* headers ship as declaration-only and
# need a stub archive to satisfy the link).  Drives `nidgen archive` for each
# YAML in $STUB_YAMLS, then installs the resulting archive under
# $PS3DEV/psl1ght/ppu/lib/ so samples can link with `-l<archive_name>_stub`.
#
# Exception: libgcm_sys_stub.a is installed into $PS3DK/ppu/lib/ as
# libgcm_sys.a (renamed — _stub suffix dropped) so `-lgcm_sys` in sample
# Makefiles finds our nidgen-generated archive first (via -L$(PS3DK)/ppu/lib
# ordering) and shadows PSL1GHT's legacy libgcm_sys.a.  This cuts the last
# PSL1GHT bytes from the rendering path at the binary level.
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

# Stub-archive YAMLs.  One entry per zero-PSL1GHT-coverage library.
STUB_YAMLS=(
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_screenshot_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_ap_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysmodule_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libl10n_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_subdisplay_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_music_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_music_decode_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_music_export_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_imejp_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libgcm_sys_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libaudio_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libpngdec_stub.yaml"
)

OUT_ROOT="$PS3_TOOLCHAIN_ROOT/build/stub-archives"
INSTALL_LIB_DEFAULT="$PS3DEV/psl1ght/ppu/lib"
INSTALL_LIB_PS3DK="$PS3DK/ppu/lib"

mkdir -p "$OUT_ROOT" "$INSTALL_LIB_DEFAULT" "$INSTALL_LIB_PS3DK"

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

    # libgcm_sys_stub.a gets a special home + rename: it lives in
    # $PS3DK/ppu/lib/ under the PSL1GHT-compat filename libgcm_sys.a so
    # -lgcm_sys in sample Makefiles finds our nidgen archive first (the
    # ppu_rules -L order places $PS3DK before $PSL1GHT), shadowing PSL1GHT's
    # legacy libgcm_sys.a entirely.  Samples then pull zero bytes from the
    # PSL1GHT tree for the GCM surface.
    #
    # We also fold in sdk/libgcm_sys_legacy/build/gcm_legacy_wrappers.o so
    # the combined archive resolves both the Sony cellGcm* names (from
    # nidgen) and the PSL1GHT gcm* names that <cell/gcm.h>'s static-inline
    # forwarders still emit — until the cell-SDK-primary surface reaches
    # everywhere and those forwarders go away.
    if [[ "$name" == "libgcm_sys_stub" ]]; then
        legacy_dir="$PS3_TOOLCHAIN_ROOT/sdk/libgcm_sys_legacy"
        say "building legacy-name wrappers (libgcm_sys_legacy)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" make -C "$legacy_dir" all >/dev/null
        legacy_obj="$legacy_dir/build/gcm_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$INSTALL_LIB_PS3DK/libgcm_sys.a"
        # Start from the nidgen archive so its object layout is preserved,
        # then ar-r the legacy wrappers on top.  'ar r' updates members in
        # place, so reruns replace rather than duplicate.
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        say "installed libgcm_sys.a -> $INSTALL_LIB_PS3DK/ (nidgen + legacy wrappers)"
    else
        install -m 0644 "${produced[0]}" "$INSTALL_LIB_DEFAULT/"
        say "installed $(basename "${produced[0]}") -> $INSTALL_LIB_DEFAULT/"
    fi
done

say "done — ${#STUB_YAMLS[@]} stub archive(s) installed"

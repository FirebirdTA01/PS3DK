#!/usr/bin/env bash
# PS3 Custom Toolchain — stub-only cell-SDK libraries
#
# Builds self-contained `lib<name>_stub.a` archives for libraries that have no
# PSL1GHT runtime backing (i.e. cell/* headers ship as declaration-only and
# need a stub archive to satisfy the link).  Drives `nidgen archive` for each
# YAML in $STUB_YAMLS, then installs the resulting archive under
# $PS3DK/ppu/lib/ so samples can link with `-l<archive_name>_stub`.
#
# Canonical file names follow the reference-SDK convention (lib*_stub.a).
# For Class 3 libraries (gcm_sys, io) that combine nidgen SPRX trampolines
# with PSL1GHT-name forwarders, the combined archive is installed under the
# reference-SDK _stub name; a symlink aliases the PSL1GHT legacy name back
# to it.  -L$(PS3DK)/ppu/lib ordering ensures the symlink shadows any
# original PSL1GHT copy.  Reference Makefiles get the _stub name directly;
# PSL1GHT-style homebrew follows the symlink transparently.
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
[[ -d "$PS3DK/ppu/lib" ]] \
    || die "PSL1GHT runtime not installed at \$PS3DK/ppu/lib. Run scripts/build-psl1ght.sh first."

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
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libio_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libjpgdec_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libspurs_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libspurs_jq_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_audio_out_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/librtc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsync_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsync2_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_savedata_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_savedata_extra_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/liblv2_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libfiber_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libusbd_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libdmux_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_oskdialog_ext_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsysutil_search_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libcrashdump_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libpamf_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libadec_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libvpost_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libatrac3plus_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libatrac3multi_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libcelpenc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libcelp8enc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsail_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libsail_rec_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libpngenc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libjpgenc_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/extracted/libgifdec_stub.yaml"
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/cellFs.yaml"
)

OUT_ROOT="$PS3_TOOLCHAIN_ROOT/build/stub-archives"
# After the PSL1GHT->PS3DK install consolidation both targets resolve to
# the same directory, but the script still installs to both names so any
# downstream Makefile referring to either still works.
INSTALL_LIB_DEFAULT="$PS3DK/ppu/lib"
INSTALL_LIB_PS3DK="$PS3DK/ppu/lib"

mkdir -p "$OUT_ROOT" "$INSTALL_LIB_DEFAULT" "$INSTALL_LIB_PS3DK" "$INSTALL_LIB_PS3DK/lp64"

# Build stub archives for both ILP32 (default) and LP64 (-mlp64) multilib.
# LP64 archives go under $PS3DK/ppu/lib/lp64/ so GCC's multilib search
# picks them up automatically when linking with -mlp64.
for abi in ilp32 lp64; do
    case $abi in
        ilp32)
            install_dir="$INSTALL_LIB_PS3DK"
            out_subdir="$OUT_ROOT/$abi"
            abi_flag=""
            cc_flags=""
            ;;
        lp64)
            install_dir="$INSTALL_LIB_PS3DK/lp64"
            out_subdir="$OUT_ROOT/$abi"
            abi_flag="--abi lp64"
            cc_flags="-mlp64"
            ;;
    esac
    mkdir -p "$out_subdir" "$install_dir"

for yaml in "${STUB_YAMLS[@]}"; do
    [[ -f "$yaml" ]] || die "missing YAML: $yaml"
    name="$(basename "$yaml" .yaml)"
    out_dir="$OUT_ROOT/$name"
    mkdir -p "$out_dir"

    say "building $name ($abi)"
    "$NIDGEN_BIN" archive \
        --input "$yaml" \
        --toolchain-bin "$PS3DEV/ppu/bin" \
        --out-dir "$out_subdir/$name" \
        $abi_flag

    # Find the produced archive (basename comes from the YAML's archive_name
    # or library field — easier to glob than to re-derive).
    shopt -s nullglob
    produced=( "$out_subdir/$name"/lib*_stub.a )
    shopt -u nullglob
    [[ ${#produced[@]} -eq 1 ]] \
        || die "expected exactly one archive in $out_dir, got ${#produced[@]}"

    # libgcm_sys_stub.a: installed under the canonical reference-SDK
    # name libgcm_sys_stub.a, with a libgcm_sys.a symlink aliasing back
    # for PSL1GHT compatibility.  -L$(PS3DK)/ppu/lib ordering ensures the
    # symlink shadows any original PSL1GHT copy; reference Makefiles get
    # the _stub name directly.
    #
    # The combined archive folds in sdk/libgcm_sys_legacy/ wrappers so it
    # resolves both Sony cellGcm* names (nidgen) and PSL1GHT gcm* names
    # that <cell/gcm.h>'s static-inline forwarders still emit — until the
    # cell-SDK-primary surface reaches everywhere and those forwarders go
    # away.
    if [[ "$name" == "libgcm_sys_stub" ]]; then
        legacy_dir="$PS3_TOOLCHAIN_ROOT/sdk/libgcm_sys_legacy"
        say "building legacy-name wrappers (libgcm_sys_legacy, $abi)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" CFLAGS="$cc_flags" \
            make -C "$legacy_dir" clean all >/dev/null
        legacy_obj="$legacy_dir/build/gcm_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$install_dir/libgcm_sys_stub.a"
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        ln -sf libgcm_sys_stub.a "$install_dir/libgcm_sys.a"
        say "installed libgcm_sys_stub.a + libgcm_sys.a symlink -> $install_dir/"
    elif [[ "$name" == "libio_stub" ]]; then
        legacy_dir="$PS3_TOOLCHAIN_ROOT/sdk/libio_legacy"
        say "building legacy-name wrappers (libio_legacy, $abi)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" CFLAGS="$cc_flags" \
            make -C "$legacy_dir" clean all >/dev/null
        legacy_obj="$legacy_dir/build/io_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$install_dir/libio_stub.a"
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        ln -sf libio_stub.a "$install_dir/libio.a"
        say "installed libio_stub.a + libio.a symlink -> $install_dir/"
    elif [[ "$name" == "libc_stub" ]]; then
        extras_dir="$PS3_TOOLCHAIN_ROOT/sdk/libc_stub_extras"
        say "building libc_stub_extras (real spu_printf_*, $abi)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" PSL1GHT="$PS3DK" CFLAGS="$cc_flags" \
            make -C "$extras_dir" clean all >/dev/null
        for extras_obj in "$extras_dir/build/"*.o; do
            [[ -f "$extras_obj" ]] || continue
            "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "${produced[0]}" \
                "$extras_obj" 2>/dev/null
        done
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "${produced[0]}"
        install -m 0644 "${produced[0]}" "$install_dir/"
        say "installed libc_stub.a -> $install_dir/ (nidgen + extras)"
    elif [[ "$name" == "libfiber_stub" ]]; then
        extras_dir="$PS3_TOOLCHAIN_ROOT/sdk/libfiber_stub_extras"
        say "building libfiber_stub_extras (pub init + TLS area, $abi)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" PSL1GHT="$PS3DK" \
            PS3_TOOLCHAIN_ROOT="$PS3_TOOLCHAIN_ROOT" CFLAGS="$cc_flags" \
            make -C "$extras_dir" clean all >/dev/null
        for extras_obj in "$extras_dir/build/"*.o; do
            [[ -f "$extras_obj" ]] || continue
            "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "${produced[0]}" \
                "$extras_obj" 2>/dev/null
        done
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "${produced[0]}"
        install -m 0644 "${produced[0]}" "$install_dir/"
        say "installed libfiber_stub.a -> $install_dir/ (nidgen + extras)"
    elif [[ "$name" == "libusbd_stub" ]]; then
        legacy_dir="$PS3_TOOLCHAIN_ROOT/sdk/libusb_legacy"
        say "building legacy-name wrappers (libusb_legacy, $abi)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" PSL1GHT="$PS3DK" \
            PS3_TOOLCHAIN_ROOT="$PS3_TOOLCHAIN_ROOT" CFLAGS="$cc_flags" \
            make -C "$legacy_dir" clean all >/dev/null
        legacy_obj="$legacy_dir/build/usb_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$install_dir/libusbd_stub.a"
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        ln -sf libusbd_stub.a "$install_dir/libusb.a"
        say "installed libusbd_stub.a + libusb.a symlink -> $install_dir/"
    else
        install -m 0644 "${produced[0]}" "$install_dir/"
        say "installed $(basename "${produced[0]}") -> $install_dir/"
    fi
done

done   # close outer abi loop

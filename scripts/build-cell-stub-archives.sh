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
    "$PS3_TOOLCHAIN_ROOT/tools/nidgen/nids/cellFs.yaml"
)

OUT_ROOT="$PS3_TOOLCHAIN_ROOT/build/stub-archives"
# After the PSL1GHT->PS3DK install consolidation both targets resolve to
# the same directory, but the script still installs to both names so any
# downstream Makefile referring to either still works.
INSTALL_LIB_DEFAULT="$PS3DK/ppu/lib"
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
        say "building legacy-name wrappers (libgcm_sys_legacy)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" make -C "$legacy_dir" all >/dev/null
        legacy_obj="$legacy_dir/build/gcm_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$INSTALL_LIB_PS3DK/libgcm_sys_stub.a"
        # Start from the nidgen archive so its object layout is preserved,
        # then ar-r the legacy wrappers on top.  'ar r' updates members in
        # place, so reruns replace rather than duplicate.
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        ln -sf libgcm_sys_stub.a "$INSTALL_LIB_PS3DK/libgcm_sys.a"
        say "installed libgcm_sys_stub.a + libgcm_sys.a symlink -> $INSTALL_LIB_PS3DK/"
    elif [[ "$name" == "libio_stub" ]]; then
        # Canonical reference-SDK libio_stub.a contains nidgen SPRX
        # stubs under Sony cellPad / cellKb / cellMouse names, plus
        # libio_legacy wrappers that forward PSL1GHT-style ioPad / ioKb /
        # ioMouse calls to the same cell* stubs.  Installed under the
        # _stub name; a libio.a symlink aliases back for PSL1GHT compat.
        legacy_dir="$PS3_TOOLCHAIN_ROOT/sdk/libio_legacy"
        say "building legacy-name wrappers (libio_legacy)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" make -C "$legacy_dir" all >/dev/null
        legacy_obj="$legacy_dir/build/io_legacy_wrappers.o"
        [[ -f "$legacy_obj" ]] \
            || die "legacy wrappers object missing after build: $legacy_obj"

        target="$INSTALL_LIB_PS3DK/libio_stub.a"
        install -m 0644 "${produced[0]}" "$target"
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "$target" "$legacy_obj" 2>/dev/null
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "$target"
        ln -sf libio_stub.a "$INSTALL_LIB_PS3DK/libio.a"
        say "installed libio_stub.a + libio.a symlink -> $INSTALL_LIB_PS3DK/"
    elif [[ "$name" == "libc_stub" ]]; then
        # The reference SDK ships spu_printf_{initialize,finalize,
        # attach_group,attach_thread,detach_group,detach_thread} in
        # libc.sprx, but the actual SPRX-side / RPCS3 HLE
        # implementation does not connect the SPU thread group's
        # printf event-port (port 1) via
        # sys_spu_thread_group_connect_event_all_threads — so SPU
        # printfs silently drop with CELL_ENOTCONN.  The yaml has
        # those six entries removed; this branch builds a real
        # PPU-side replacement out of sdk/libc_stub_extras and
        # ar-appends it into libc_stub.a so the resulting archive
        # has the same six user-facing names but with a working
        # event-queue + connect_event_all_threads handler behind
        # them.
        extras_dir="$PS3_TOOLCHAIN_ROOT/sdk/libc_stub_extras"
        say "building libc_stub_extras (real spu_printf_*)"
        PS3DEV="$PS3DEV" PS3DK="$PS3DK" PSL1GHT="$PS3DK" \
            make -C "$extras_dir" all >/dev/null
        for extras_obj in "$extras_dir/build/"*.o; do
            [[ -f "$extras_obj" ]] || continue
            "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" r "${produced[0]}" \
                "$extras_obj" 2>/dev/null
        done
        "$PS3DEV/ppu/bin/powerpc64-ps3-elf-ranlib" "${produced[0]}"
        install -m 0644 "${produced[0]}" "$INSTALL_LIB_DEFAULT/"
        say "installed libc_stub.a -> $INSTALL_LIB_DEFAULT/ (nidgen + extras)"
    else
        install -m 0644 "${produced[0]}" "$INSTALL_LIB_DEFAULT/"
        say "installed $(basename "${produced[0]}") -> $INSTALL_LIB_DEFAULT/"
    fi
done

# libusb.a is built by build-psl1ght.sh (PSL1GHT's libpsl1ght/libusb);
# it has no nidgen counterpart yet.  Reference Makefiles spell it as
# libusbd_stub.a, so alias the PSL1GHT name to satisfy -lusbd_stub.
# TODO: extract a libusbd_stub.yaml, generate a real libusbd_stub.a via
# nidgen, and invert the symlink direction (libusb.a → libusbd_stub.a).
libusb="$INSTALL_LIB_PS3DK/libusb.a"
if [[ -f "$libusb" ]]; then
    ln -sf libusb.a "$INSTALL_LIB_PS3DK/libusbd_stub.a"
    say "installed libusbd_stub.a symlink -> libusb.a"
else
    say "warning: libusb.a not found; skip libusbd_stub.a symlink"
fi

say "done — ${#STUB_YAMLS[@]} stub archive(s) installed"

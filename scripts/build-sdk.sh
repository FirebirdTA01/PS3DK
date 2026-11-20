#!/usr/bin/env bash
# PS3 Custom Toolchain — build + install our SDK.
#
# Single entry point for the entire sdk/ tree: libraries plus header-
# only subsystems.  Runs after the PSL1GHT runtime is in place
# (build-psl1ght.sh), since today's SDK headers forward to PSL1GHT
# symbols.  As we wean off PSL1GHT, the forwarding details change
# behind this command but the interface does not.
#
#   source ./scripts/env.sh
#   ./scripts/build-sdk.sh
#   ./scripts/build-sdk.sh --no-host-tools  # SDK-only rebuild

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"
# The compiler adds the SDK include directory by default (gcc-16.x patch 0068); building the
# toolchain, the runtime or the SDK itself must never see an installed SDK's headers.
export PS3DK_NO_DEFAULT_INCLUDE=1

say() { printf "[sdk] %s\n" "$*"; }
die() { printf "[sdk] ERROR: %s\n" "$*" >&2; exit 1; }

HEADERS_ONLY=false
INSTALL_HOST_TOOLS=true
for arg in "$@"; do
    case "$arg" in
        --headers-only) HEADERS_ONLY=true; INSTALL_HOST_TOOLS=false ;;
        --no-host-tools) INSTALL_HOST_TOOLS=false ;;
        *) die "unknown argument: $arg" ;;
    esac
done

[[ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]] \
    || die "PPU toolchain not installed. Run scripts/build-ppu-toolchain.sh first."
[[ -d "$PS3DK/ppu/include" ]] \
    || die "PSL1GHT runtime not installed at \$PS3DK/ppu/include. Run scripts/build-psl1ght.sh first."

if $HEADERS_ONLY; then
    say "installing SDK headers only"
    make -C "$PS3_TOOLCHAIN_ROOT/sdk" install-headers install-spu-headers install-shared-spurs-spu-headers install-version
    say "done"
    exit 0
fi

# The host tools go in BEFORE the SDK build: libdbgfont compiles its shaders
# with rsx-cg-compiler during `make install` below, and install-host-tools.sh is
# what builds and installs that compiler.  The host tools build from their own
# sources and need nothing from the SDK build.
if $INSTALL_HOST_TOOLS; then
    say "building + installing host tools"
    "$PS3_TOOLCHAIN_ROOT/scripts/install-host-tools.sh"
else
    say "skipping host tools (--no-host-tools)"
    # Ask libdbgfont's own lookup (tools/rsx-cg-compiler/build, $PS3DEV/bin,
    # $PS3DK/bin; not PATH), so this check cannot disagree with the build.
    make -s -C "$PS3_TOOLCHAIN_ROOT/sdk/libdbgfont" rsx-cg-path >/dev/null 2>&1 \
        || die "rsx-cg-compiler not found in tools/rsx-cg-compiler/build, \$PS3DEV/bin or \$PS3DK/bin, and --no-host-tools was given; run scripts/build-sdk.sh without --no-host-tools once (or scripts/install-host-tools.sh) to build it."
fi

say "building + installing SDK"
# The SDK's own sublibs compile with -I../include AND -I$PS3DK/ppu/include, so
# two copies of the libc wrapper headers (stdlib.h, sys/types.h, ...) sit on
# the include path.  The wrappers forward with #include_next outside their
# include guards, so each copy chains on to libc's header.
make -C "$PS3_TOOLCHAIN_ROOT/sdk" install

# The profiler's SPU/RSX extension ships for both data models (director
# decision D1): the libps3dkprof sublib's install above puts the PROFILE-form
# libps3dkprof_ext.a, libps3dkprof_ext_live.a (ppu/lib and ppu/lib/lp64), the
# ppu/include/ps3dk extension headers, and the SPU lane library
# spu/lib/libps3dkprof_spu.a with its spu/include/ps3dk headers in place,
# re-verifying each archive.
#
# The opt-in profiler packages are not part of the sublib install: each one is
# an explicit target of sdk/libps3dkprof (and libgcm_cmd for AUTO), and an app
# selects it with its own CMake option (docs/profiler-guide.md section 4).
# They are installed here, for both data models, so that every SDK built by
# this script -- the release included -- carries them.  Order matters: the
# function-path archives go in before the extension check below, the AUTO
# package last, because install-auto-package seals the installed providers
# and the headers they were built against.
PROF=(make -C "$PS3_TOOLCHAIN_ROOT/sdk/libps3dkprof" --no-print-directory)
GCMD=(make -C "$PS3_TOOLCHAIN_ROOT/sdk/libgcm_cmd" --no-print-directory)
PPU_PREFIX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-"

# The source commit recorded by the observed runtime and the AUTO seal.  A
# build outside a git checkout (a source tarball) names it explicitly.
prof_source_commit="${PS3DK_PROF_SOURCE_COMMIT:-}"
if [[ -z "$prof_source_commit" ]]; then
    prof_source_commit="$(git -C "$PS3_TOOLCHAIN_ROOT" rev-parse HEAD)" \
        || die "cannot read the source commit; outside a git checkout set PS3DK_PROF_SOURCE_COMMIT to the full commit the sources come from"
fi
[[ "$prof_source_commit" =~ ^[0-9a-f]{40}$ ]] \
    || die "profiler source commit is not a full commit id: $prof_source_commit"

say "building + installing the profiler function-path packages (ILP32 + LP64)"
"${PROF[@]}" install-func install-func-ext || die "profiler install-func/install-func-ext failed"

# Check the INSTALLED tree again here, from the installed files alone, so a
# sublib install that skipped or lost the extension fails the SDK build.
say "verifying the installed profiler extension (ILP32 + LP64)"
"${PROF[@]}" verify-install-ext \
    || die "installed profiler extension failed verify-install-ext"

# Wait observations (PS3_PROF_WAITS).  The WAIT_* footprints are the measured
# inputs the archives are checked against (waits.mk has no default on
# purpose); the observed pthread runtime goes next to them.
say "building + installing the profiler wait packages and the observed runtime (ILP32 + LP64)"
"${PROF[@]}" install-waits \
    WAIT_STACK_ILP32=1024 WAIT_STACK_LP64=1024 WAIT_TLS_ILP32=4 WAIT_TLS_LP64=4 \
    WAIT_STATIC_ILP32=24 WAIT_STATIC_LP64=24 WAIT_STATIC_EXT_ILP32=16 WAIT_STATIC_EXT_LP64=24 \
    || die "profiler install-waits failed"
[[ -n "$PS3_BUILD_ROOT" ]] || die "PS3_BUILD_ROOT is empty"
wait_runtime_out="$PS3_BUILD_ROOT/sdk/prof-wait-runtime"
rm -rf "$wait_runtime_out"
python3 "$PS3_TOOLCHAIN_ROOT/scripts/build-prof-wait-runtime.py" \
    --compiler "${PPU_PREFIX}gcc" --sdk "$PS3DK" --out "$wait_runtime_out" \
    --install-prefix "$PS3DK" --source-base "$prof_source_commit" \
    || die "observed wait runtime build failed"

say "building + installing the profiler counter packages (ILP32 + LP64)"
"${PROF[@]}" install-counters || die "profiler install-counters failed"

# AUTO RSX observation (PS3_PROF_AUTO): the GCM command provider, then the
# profiler provider, sealed together in prof-auto.json per ABI.  The seal
# reinstalls libps3dkprof_ext_live.a; it must be byte-identical to the one the
# sublib install put there, so keep a copy and compare.
say "building + installing the AUTO profiler package (ILP32 + LP64)"
"${GCMD[@]}" install-auto || die "libgcm_cmd install-auto failed"
ext_live_before="$PS3_BUILD_ROOT/sdk/prof-ext-live-before"
rm -rf "$ext_live_before"
install -d "$ext_live_before/ilp32" "$ext_live_before/lp64"
install -m 0644 "$PS3DK/ppu/lib/libps3dkprof_ext_live.a" "$ext_live_before/ilp32/"
install -m 0644 "$PS3DK/ppu/lib/lp64/libps3dkprof_ext_live.a" "$ext_live_before/lp64/"
"${PROF[@]}" install-auto-package AUTO_SOURCE_COMMIT="$prof_source_commit" PPU_READELF="${PPU_PREFIX}readelf" \
    || die "profiler install-auto-package failed"
cmp "$ext_live_before/ilp32/libps3dkprof_ext_live.a" "$PS3DK/ppu/lib/libps3dkprof_ext_live.a" \
    || die "the AUTO package changed the installed ILP32 libps3dkprof_ext_live.a"
cmp "$ext_live_before/lp64/libps3dkprof_ext_live.a" "$PS3DK/ppu/lib/lp64/libps3dkprof_ext_live.a" \
    || die "the AUTO package changed the installed LP64 libps3dkprof_ext_live.a"
for abi in ilp32 lp64; do
    python3 "$PS3DK/cmake/ps3-prof-auto-check.py" --root "$PS3DK" --abi "$abi" \
        || die "installed AUTO package failed its $abi check"
done

# Replace PSL1GHT's default ICON0.PNG with our PS3DK-branded one.
# Samples that don't override ICON0 in their Makefile pick up whatever
# lives at $PS3DEV/bin/ICON0.PNG; this install makes that our icon
# instead of the PSL1GHT logo PSL1GHT itself drops there during its
# own tool install.  Re-run this after any build-psl1ght.sh since that
# restores the PSL1GHT default on top of us.
#
# It is installed TWICE, to two different consumers:
#   $PS3DEV/bin/ICON0.PNG      what PSL1GHT-era Makefile samples read via
#                              ppu_rules' `ICON0 ?= $(PS3DEV)/bin/ICON0.PNG`.
#   $PS3DK/sdk/assets/ICON0.PNG  where ps3_add_pkg()'s default icon path
#                              resolves inside an INSTALLED tree -- cmake/
#                              ps3-self.cmake sets _PS3_TOOLCHAIN_ROOT from its
#                              own location, so in an installed SDK that path
#                              is $PS3DK/sdk/assets/ICON0.PNG.
# The second copy is also the manifest's `sdk_core sdk/assets/ICON0.PNG` row,
# which the cmake freshness probe FATAL_ERRORs on.  Until this line existed the
# only writer of that path was scripts/package-windows-release.sh, so the row
# was true in a packaged tree and false in every from-source install: the probe
# refused to configure until someone hand-staged the file.  A promise in the
# manifest has to be kept by the install phase, not only by the packager.
# Both copies are installed UNCONDITIONALLY, and a missing source icon is
# fatal.  This used to sit behind an `if [[ -f ]]`, which meant a tree without
# sdk/assets/ICON0.PNG produced a green install that was missing a REQUIRED
# manifest row -- the failure then surfaced far away, as the cmake probe
# refusing to configure, or as ps3_add_pkg() FATAL_ERRORing on a sample.  A
# guard that skips a required artifact is the silent-no-op class we keep
# getting bitten by.  scripts/package-windows-release.sh already dies on the
# same condition; the source-install path now matches it.
icon_src="$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG"
[[ -f "$icon_src" ]] \
    || die "sdk/assets/ICON0.PNG missing: $icon_src — it is a required manifest row (sdk_core) and ps3_add_pkg() defaults every sample icon to it."
install -m 0644 "$icon_src" "$PS3DEV/bin/ICON0.PNG"
say "installed sdk/assets/ICON0.PNG -> $PS3DEV/bin/ICON0.PNG"
install -d "$PS3DK/sdk/assets"
install -m 0644 "$icon_src" "$PS3DK/sdk/assets/ICON0.PNG"
say "installed sdk/assets/ICON0.PNG -> $PS3DK/sdk/assets/ICON0.PNG"

# Verify the installed GCM surface, now that BOTH halves of it exist.  The
# archive checks need libgcm_sys.a (built by build-cell-stub-archives.sh) AND
# libgcm_cmd.a (built above, in SUBLIBS), so this is the earliest point where
# the tree is complete enough to check.  Running it from the stub-archive
# script instead - where it lived first - reported the four default-FIFO
# functions in libgcm_cmd.a as missing, because that archive genuinely does
# not exist yet at that point, and it failed a release build.
say "verifying the installed legacy gcm* surface"
PS3DK="$PS3DK" PS3DEV="$PS3DEV" \
    bash "$PS3_TOOLCHAIN_ROOT/tests/sdk/gcm-legacy-symbol-coverage-test.sh" \
    || die "installed archives failed tests/sdk/gcm-legacy-symbol-coverage-test.sh"

say "done"

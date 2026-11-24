#!/usr/bin/env bash
# build.sh [BUILD_DIR]: the console side of ps3dk-run (the loader PKG and the test child) with the installed SDK's
# toolchain ($PS3DK/cmake, else this checkout's cmake/).  The host tool builds with the tools/ Rust workspace
# (cargo build -p ps3dk-run, or scripts/build-sdk.sh's host tools).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd "$here/../.." && pwd -P)"
die() { printf "[ps3dk-run] ERROR: %s\n" "$*" >&2; exit 1; }
[[ -n "${PS3DK:-}" ]] || die "PS3DK is not set (source scripts/env.sh, or point it at an installed SDK)"
toolchain="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
[[ -f "$toolchain" ]] || toolchain="$root/cmake/ps3-ppu-toolchain.cmake"
build="${1:-${PS3_BUILD_ROOT:-$HOME/ps3tc/build}/ps3dk-run}"
gen=()
command -v ninja >/dev/null 2>&1 && gen=(-G Ninja)
cmake -S "$here" -B "$build" "${gen[@]}" -DCMAKE_TOOLCHAIN_FILE="$toolchain"
cmake --build "$build"
printf "[ps3dk-run] loader PKG: %s\n" "$build/loader/ps3dk-loader.pkg"
printf "[ps3dk-run] test child: %s\n" "$build/child/ps3dk-run-child.self"

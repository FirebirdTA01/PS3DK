#!/usr/bin/env bash
# PS3 Custom Toolchain - build and install Linux host tools.
#
# Installs repo-owned host tools into $PS3DEV/bin and mirrors them into
# $PS3DK/bin so CMake find_program() consumers can use the staged tools
# without in-tree fallback paths.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[host-tools] %s\n" "$*"; }
die() { printf "[host-tools] ERROR: %s\n" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: scripts/install-host-tools.sh

Builds and installs Linux host tools into $PS3DEV/bin, with a mirror
under $PS3DK/bin.
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $arg" ;;
    esac
done

TOOLS_DIR="$PS3_TOOLCHAIN_ROOT/tools"
INSTALL_BINS=("$PS3DEV/bin" "$PS3DK/bin")

JOBS="${JOBS:-}"
if [[ -z "$JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    else
        JOBS=4
    fi
fi

install_tool() {
    local src="$1"
    local name="$2"

    [[ -x "$src" ]] || die "host tool missing or not executable: $src"
    for dir in "${INSTALL_BINS[@]}"; do
        install -d "$dir"
        install -m 0755 "$src" "$dir/$name"
        say "installed $name -> $dir/$name"
    done
}

build_rust_workspace() {
    say "building Rust workspace (release)"
    cargo build --release --workspace --manifest-path "$TOOLS_DIR/Cargo.toml"

    local out="$TOOLS_DIR/target/release"
    local bin
    for bin in nidgen coverage-report abi-verify spu-elf-to-ppu-obj; do
        install_tool "$out/$bin" "$bin"
    done
}

build_rsx_cg_compiler() {
    say "building rsx-cg-compiler"
    local src="$TOOLS_DIR/rsx-cg-compiler"
    local build="$PS3_BUILD_ROOT/host-tools-linux/rsx-cg-compiler"

    cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --target rsx-cg-compiler -j "$JOBS"

    install_tool "$build/rsx-cg-compiler" "rsx-cg-compiler"
}

build_sprx_linker() {
    say "building sprxlinker"
    make -C "$TOOLS_DIR/sprx-linker" install PS3DEV="$PS3DEV" PS3DK="$PS3DK"
}

[[ -d "$PS3DEV" ]] || die "PS3DEV does not exist: $PS3DEV (run scripts/bootstrap.sh first)"
[[ -d "$PS3DK" ]] || install -d "$PS3DK"

build_rust_workspace
build_rsx_cg_compiler
build_sprx_linker

say "done"

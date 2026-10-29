#!/usr/bin/env bash
# PS3 Custom Toolchain — assert that out-of-tree sample builds keep samples/ clean.
#
# Verifies that building samples with ps3_add_self and ps3_add_pkg (including
# hello-ppu-png and hello-ppu-camera) across both ABIs (ILP32 and LP64) leaves
# the source directory completely pristine (no generated outputs or assets).
#
# Uses an explicit filesystem snapshot (path, size, sha256) including gitignored
# files (*.self, *.sprx, etc.) and git status --porcelain --ignored, with a
# deliberate injected leak red/green control.
#
# usage: samples-clean-build-test.sh [--ps3dev DIR]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
ps3dev="${PS3DEV:-${PS3DK:-}}"

while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done

snapshot_samples() {
    local out="$1"
    (
        cd "$repo_root"
        find samples/ -type f -exec sha256sum {} + | sort -k2
    ) > "$out"
}

check_samples_clean() {
    local baseline_snap="$1"
    local current_snap="$tmp/current_snap.txt"
    snapshot_samples "$current_snap"
    if ! diff -u "$baseline_snap" "$current_snap" > "$tmp/snap_diff.txt" 2>&1; then
        echo "samples-clean-build: filesystem diff detected in samples/:" >&2
        cat "$tmp/snap_diff.txt" >&2
        return 1
    fi
    return 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

baseline_snap="$tmp/baseline_snap.txt"
snapshot_samples "$baseline_snap"

# Self-check / Red-control: prove the check fails when an ignored output leaks
leak_probe="$repo_root/samples/vision/hello-ppu-camera/witness.fake.self"
touch "$leak_probe"
if check_samples_clean "$baseline_snap" >/dev/null 2>&1; then
    rm -f "$leak_probe"
    echo "samples-clean-build: FAIL: red control failed to detect leaked ignored file" >&2
    exit 1
fi
rm -f "$leak_probe"

if ! check_samples_clean "$baseline_snap"; then
    echo "samples-clean-build: FAIL: samples directory dirty before build" >&2
    exit 1
fi

if [ -z "$ps3dev" ]; then
    echo "samples-clean-build: SKIP (set PS3DEV or --ps3dev; red control verified)"
    exit 0
fi

to_cmake_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

cmake_ps3dev="$(to_cmake_path "$ps3dev")"
cc_ppu="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
if [ ! -f "$cc_ppu" ]; then
    echo "samples-clean-build: SKIP (no Linux PPU compiler at $cc_ppu; red control verified)"
    exit 0
fi

cmake_gen=()
if command -v ninja >/dev/null 2>&1; then
    cmake_gen=(-G Ninja)
fi

ilp32_tc="$(to_cmake_path "$repo_root/cmake/ps3-ppu-toolchain.cmake")"
lp64_tc="$(to_cmake_path "$repo_root/cmake/ps3-ppu-toolchain-lp64.cmake")"

build_sample() {
    local sample_rel="$1"
    local abi="$2"
    local tc="$3"
    local bld="$tmp/bld-${sample_rel//\//_}-$abi"

    local src="$(to_cmake_path "$repo_root/samples/$sample_rel")"
    local cmake_bld="$(to_cmake_path "$bld")"

    if ! cmake -S "$src" -B "$cmake_bld" \
        "${cmake_gen[@]}" \
        "-DCMAKE_TOOLCHAIN_FILE=$tc" \
        "-DPS3DEV=$cmake_ps3dev" \
        "-DPS3DK=$cmake_ps3dev" > "$tmp/cfg-$abi.log" 2>&1; then
        cat "$tmp/cfg-$abi.log" >&2
        echo "samples-clean-build: FAIL configure $sample_rel ($abi)" >&2
        exit 1
    fi

    if ! cmake --build "$cmake_bld" -j 2 > "$tmp/bld-$abi.log" 2>&1; then
        cat "$tmp/bld-$abi.log" >&2
        echo "samples-clean-build: FAIL build $sample_rel ($abi)" >&2
        exit 1
    fi
}

# Build hello-ppu-png and hello-ppu-camera for ILP32 and LP64
build_sample "sysutil/hello-ppu-png" "ilp32" "$ilp32_tc"
build_sample "sysutil/hello-ppu-png" "lp64"  "$lp64_tc"
build_sample "vision/hello-ppu-camera" "ilp32" "$ilp32_tc"
build_sample "vision/hello-ppu-camera" "lp64"  "$lp64_tc"

# Assert no files changed or appeared in samples/
if ! check_samples_clean "$baseline_snap"; then
    echo "samples-clean-build: FAIL: sample build leaked files into samples/" >&2
    exit 1
fi

echo "samples-clean-build: PASS (samples/ source tree remains completely clean across both ABIs)"

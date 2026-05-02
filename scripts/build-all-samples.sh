#!/usr/bin/env bash
# PS3 Custom Toolchain — build all samples
#
# Iterates over every sample directory that contains a CMakeLists.txt,
# configures and builds each one with the PPU toolchain.  SPU samples
# that include the SPU toolchain (or have a separate SPU subdirectory)
# are handled by their own CMakeLists.
#
# Usage:
#   source ./scripts/env.sh
#   ./scripts/build-all-samples.sh              # build everything
#   ./scripts/build-all-samples.sh --rebuild    # clean + rebuild
#   ./scripts/build-all-samples.sh gcm          # only samples/gcm/*
#   ./scripts/build-all-samples.sh sysutil      # only samples/sysutil/*

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[samples] %s\n" "$*"; }
die() { printf "[samples] ERROR: %s\n" "$*" >&2; exit 1; }

REBUILD=false
FILTER=""

for arg in "$@"; do
    case "$arg" in
        --rebuild) REBUILD=true ;;
        *)         FILTER="$arg" ;;
    esac
done

SAMPLES_ROOT="$PS3_TOOLCHAIN_ROOT/samples"
TOOLCHAIN_FILE="$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"

[[ -d "$SAMPLES_ROOT" ]] || die "samples root not found: $SAMPLES_ROOT"
[[ -f "$TOOLCHAIN_FILE" ]] || die "toolchain file not found: $TOOLCHAIN_FILE"

FAILED=()
PASSED=0
TOTAL=0

# Collect all sample dirs (those with a CMakeLists.txt, excluding
# nested build/ and cmake-build/ subdirs).
while IFS= read -r -d '' cmake_file; do
    sample_dir="$(dirname "$cmake_file")"
    sample_name="${sample_dir#"$SAMPLES_ROOT/"}"

    # Optional filter
    if [[ -n "$FILTER" && "$sample_name" != "$FILTER"/* && "$sample_name" != "$FILTER" ]]; then
        continue
    fi

    TOTAL=$((TOTAL + 1))
    build_dir="$sample_dir/build"

    say "[$TOTAL] $sample_name"

    if $REBUILD && [[ -d "$build_dir" ]]; then
        rm -rf "$build_dir"
    fi

    mkdir -p "$build_dir"

    if ! cmake -S "$sample_dir" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        >"$build_dir/cmake.log" 2>&1; then
        say "  CMake configure FAILED — see $build_dir/cmake.log"
        FAILED+=("$sample_name (configure)")
        continue
    fi

    if ! cmake --build "$build_dir" >"$build_dir/build.log" 2>&1; then
        say "  Build FAILED — see $build_dir/build.log"
        FAILED+=("$sample_name (build)")
        continue
    fi

    PASSED=$((PASSED + 1))
    say "  OK"

done < <(find "$SAMPLES_ROOT" -name CMakeLists.txt \
    -not -path "*/build/*" \
    -not -path "*/cmake-build/*" \
    -print0)

say ""
say "==============================================================================="
say "Results: $PASSED / $TOTAL passed"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    say "Failed:"
    for f in "${FAILED[@]}"; do
        say "  - $f"
    done
    exit 1
fi

say "All samples built successfully."

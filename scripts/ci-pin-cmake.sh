#!/usr/bin/env bash
# Pin the CMake the release jobs build with.
#
# CMake writes its own version into files the SDK ships (a CMake package's
# exported targets file records `cmake_policy(VERSION 2.8.12...<major.minor>)`
# of the CMake that generated it), and the runner images carry different
# CMake versions: two release runs of one commit once got 4.5.0 and 4.4.5
# and differed in portlibs/ppu/lib/cmake/libzip/libzip-targets.cmake.  The
# release jobs therefore use one exact CMake, from Kitware's release tarball,
# checked against its published sha256.
#
#   ci-pin-cmake.sh            install it under $RUNNER_TEMP and put its bin/
#                              first on PATH for the job's later steps
#                              ($GITHUB_PATH)
#   ci-pin-cmake.sh --check    refuse unless the cmake found on PATH is that
#                              install and reports exactly the pinned version
set -euo pipefail

CMAKE_PIN_VERSION=4.4.5
CMAKE_PIN_SHA256=ed85135c5a5135b45e50423cb406dea04ca3338da0b8ef3c9ede963559680be0
CMAKE_PIN_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_PIN_VERSION}/cmake-${CMAKE_PIN_VERSION}-linux-x86_64.tar.gz"

die() { printf 'ci-pin-cmake: %s\n' "$*" >&2; exit 1; }
[[ -n "${RUNNER_TEMP:-}" ]] || die "RUNNER_TEMP is not set (this script is for the release workflow)"
dir="$RUNNER_TEMP/cmake-$CMAKE_PIN_VERSION"
want="cmake version $CMAKE_PIN_VERSION"

check() {
    local found got
    found="$(command -v cmake)" || die "no cmake on PATH"
    [[ "$found" == "$dir/bin/cmake" ]] || die "cmake on PATH is $found, not the pinned $dir/bin/cmake"
    got="$("$found" --version | head -1)"
    [[ "$got" == "$want" ]] || die "cmake reports '$got', the pin is '$want'"
    printf 'ci-pin-cmake: %s (%s)\n' "$got" "$found"
}

case "${1:-}" in
    --check) check; exit 0 ;;
    "") ;;
    *) die "usage: ci-pin-cmake.sh [--check]" ;;
esac

[[ ! -e "$dir" ]] || die "$dir already exists"
mkdir -p "$dir"
tarball="$RUNNER_TEMP/cmake-$CMAKE_PIN_VERSION-linux-x86_64.tar.gz"
curl -fsSL --retry 3 -o "$tarball" "$CMAKE_PIN_URL"
printf '%s  %s\n' "$CMAKE_PIN_SHA256" "$tarball" | sha256sum -c - \
    || die "sha256 of $CMAKE_PIN_URL is not $CMAKE_PIN_SHA256"
tar -C "$dir" --strip-components=1 -xzf "$tarball"
rm -f "$tarball"
got="$("$dir/bin/cmake" --version | head -1)"
[[ "$got" == "$want" ]] || die "installed cmake reports '$got', the pin is '$want'"
[[ -n "${GITHUB_PATH:-}" ]] || die "GITHUB_PATH is not set"
printf '%s\n' "$dir/bin" >> "$GITHUB_PATH"
printf 'ci-pin-cmake: installed %s at %s\n' "$got" "$dir"

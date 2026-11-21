#!/usr/bin/env bash
# The exact Rust toolchain the release jobs build with.
#
# The Rust tools ship in every release, and `dtolnay/rust-toolchain@stable`
# installs whatever stable is current on the day, so two release runs of one
# commit could build them with different compilers.  The release workflow
# installs this exact version (`dtolnay/rust-toolchain@<RUST_PIN_VERSION>`,
# kept equal to the value below by tests/host/ci-pin-rust-test.sh) and runs
# this script after it, as it runs scripts/ci-pin-cmake.sh --check for CMake.
#
#   ci-pin-rust.sh --check      refuse unless rustc and cargo on PATH report
#                               exactly the pinned version
#   ci-pin-rust.sh --version    print the pinned version
#
# To move the pin: change RUST_PIN_VERSION here and every
# dtolnay/rust-toolchain@ line in .github/workflows/release.yml together.
set -euo pipefail

RUST_PIN_VERSION=1.100.0

die() { printf 'ci-pin-rust: %s\n' "$*" >&2; exit 1; }

check() {
    local tool got
    for tool in rustc cargo; do
        command -v "$tool" >/dev/null || die "no $tool on PATH"
        got="$("$tool" --version)" || die "$tool --version failed"
        [[ "$got" == "$tool $RUST_PIN_VERSION "* ]] \
            || die "$tool reports '$got', the pin is '$tool $RUST_PIN_VERSION'"
        printf 'ci-pin-rust: %s (%s)\n' "$got" "$(command -v "$tool")"
    done
}

case "${1:-}" in
    --check) check ;;
    --version) printf '%s\n' "$RUST_PIN_VERSION" ;;
    *) die "usage: ci-pin-rust.sh --check | --version" ;;
esac

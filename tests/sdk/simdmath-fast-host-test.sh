#!/usr/bin/env bash
# Host test for the f4fast forms (sdk/include/simdmath/fastf4.h): sweeps each
# form over its documented domain against double-precision libm with
# pessimistic 12-bit reciprocal estimates and enforces the accuracy bounds
# the header documents (tests/sdk/simdmath-fast-host-test.c).
#
# usage: simdmath-fast-host-test.sh     (CC overrides the host compiler)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc="${CC:-cc}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/sdk/include" \
    "$root/tests/sdk/simdmath-fast-host-test.c" -o "$work/simdmath-fast" -lm
"$work/simdmath-fast"

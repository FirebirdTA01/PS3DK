#!/usr/bin/env bash
# Build and run the DS3 bench sample's native host tests (CTest).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${DS3B_TEST_OUTPUT:-$(mktemp -d)}"
if [[ -z "${DS3B_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$work"' EXIT; fi
cmake -S "$root/samples/input/ds3-bench/host-tests" -B "$work" >/dev/null
cmake --build "$work" --parallel 2 >/dev/null
ctest --test-dir "$work" --output-on-failure

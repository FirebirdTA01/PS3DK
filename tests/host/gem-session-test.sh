#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${GEM_TEST_OUTPUT:-$(mktemp -d)}"
if [[ -z "${GEM_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$work"' EXIT; fi
mkdir -p "$work"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$root/tests/host/gem-session-test.cpp" \
  "$root/samples/input/hello-ppu-gem-basic/source/gem_session.cpp" \
  -o "$work/gem-session-test"
"$work/gem-session-test"

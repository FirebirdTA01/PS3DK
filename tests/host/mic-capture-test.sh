#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${MIC_TEST_OUTPUT:-$(mktemp -d)}"
if [[ -z "${MIC_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$work"' EXIT; fi
mkdir -p "$work"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$root/tests/host/mic-meter-test.cpp" \
  "$root/samples/audio/hello-ppu-mic/source/mic_meter.cpp" -o "$work/mic-meter-test"
"$work/mic-meter-test"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$root/tests/host/mic-session-test.cpp" \
  "$root/samples/audio/hello-ppu-mic/source/mic_session.cpp" \
  "$root/samples/audio/hello-ppu-mic/source/mic_meter.cpp" -o "$work/mic-session-test"
"$work/mic-session-test"

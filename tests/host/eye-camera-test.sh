#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${EYE_TEST_OUTPUT:-$(mktemp -d)}"
if [[ -z "${EYE_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$work"' EXIT; fi
mkdir -p "$work"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
  "$root/tests/host/eye-camera-test.c" \
  "$root/samples/vision/hello-ppu-camera/source/eye_yuv.c" \
  -o "$work/eye-camera-test"
"$work/eye-camera-test"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
  "$root/tests/host/eye-capture-test.c" \
  "$root/samples/vision/hello-ppu-camera/source/eye_capture.c" \
  -o "$work/eye-capture-test"
"$work/eye-capture-test"

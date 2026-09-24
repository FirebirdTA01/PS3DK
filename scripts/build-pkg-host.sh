#!/usr/bin/env bash
# Build the native pkg tool only after its SHA-1 implementation passes.
# Shared by the Linux source installer and the Linux host-tools release job.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo 'Usage: scripts/build-pkg-host.sh <output-directory>' >&2
    exit 2
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
src="$root/tools/sfo-pkg"
out="$1"
compiler="${HOST_CC:-cc}"
mkdir -p "$out"

echo '[pkg-host] running native SHA-1 self-test'
"$compiler" -O2 -Wall -Wextra "$src/sha1.c" "$src/sha1-test.c" -o "$out/sha1-test"
if ! "$out/sha1-test"; then
    echo '[pkg-host] SHA-1 self-test failed; refusing to build pkg' >&2
    exit 1
fi
"$compiler" -O2 "$src/pkg.c" "$src/sha1.c" -o "$out/pkg"
echo "[pkg-host] built $out/pkg"

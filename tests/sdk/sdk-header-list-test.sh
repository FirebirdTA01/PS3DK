#!/usr/bin/env bash
# Every tracked header under sdk/include/ must be installed by
# `make -C sdk install-headers`.
#
# install-headers copies an explicit list, not a wildcard. A header that is
# added to the tree but not to the list is silently left out of the installed
# SDK, and the first file that includes it breaks the SDK build
# (sys/process.h -> sys/heap_config.h did exactly that). CI does not run the
# full SDK build, so this check is the early warning. It runs the real
# install-headers into a scratch directory and compares what was installed,
# so a comment or another variable naming a header cannot satisfy it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sdk="$root/sdk"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

make -s -C "$sdk" install-headers PS3DEV="$scratch/ps3dev" INSTALL_INC="$scratch/include" > /dev/null

tracked="$(cd "$sdk" && git ls-files include | sort -u)"
installed="$(cd "$scratch" && find include -type f | sort -u)"

missing="$(comm -23 <(printf '%s\n' "$tracked") <(printf '%s\n' "$installed"))"
if [ -n "$missing" ]; then
    echo "FAIL: tracked SDK headers not installed by install-headers:"
    printf '  %s\n' $missing
    exit 1
fi

count="$(printf '%s\n' "$tracked" | wc -l | tr -d ' ')"
echo "sdk-header-list: PASS ($count tracked headers, all installed)"

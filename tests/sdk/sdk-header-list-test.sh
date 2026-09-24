#!/usr/bin/env bash
# Every tracked header under sdk/include/ must be named in sdk/Makefile.
#
# `make install-headers` copies an explicit list, not a wildcard. A header
# that is added to the tree but not to the list is silently left out of the
# installed SDK, and the first file that includes it breaks the SDK build
# (sys/process.h -> sys/heap_config.h did exactly that). CI does not run the
# full SDK build, so this check is the early warning.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root/sdk"

tracked="$(git ls-files include | sort -u)"
listed="$(grep -oE 'include/[^[:space:]\\]+' Makefile | sort -u)"

missing="$(comm -23 <(printf '%s\n' "$tracked") <(printf '%s\n' "$listed"))"
if [ -n "$missing" ]; then
    echo "FAIL: tracked SDK headers not listed in sdk/Makefile (they would not be installed):"
    printf '  %s\n' $missing
    exit 1
fi

count="$(printf '%s\n' "$tracked" | wc -l | tr -d ' ')"
echo "sdk-header-list: PASS ($count tracked headers, all listed)"

#!/bin/bash
set -euo pipefail

# This script documents the one-time generation of the golden baseline fixtures
# in tests/sdk/fixtures/sfo-golden/ from the pinned pre-change sfo.c blob:
# Git blob: e7efc0bc583654135ccbaccadce76c0ad16c6129 (origin/main)
#
# It enforces an 8192 KiB stack limit bound (checking that ulimit succeeded
# and reading back the bound), builds the historical tool, and produces
# the golden fixtures.

PINNED_BLOB="e7efc0bc583654135ccbaccadce76c0ad16c6129"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
TMPDIR="$(mktemp -d /tmp/sfo_golden_gen.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

# Enforce bounded 8192 KiB stack limit (verifying setting succeeded and reading back)
ulimit -S -s 8192
CURRENT_STACK="$(ulimit -s)"
if [ "$CURRENT_STACK" != "8192" ]; then
    echo "ERROR: Failed to set stack limit to 8192 KiB (got: $CURRENT_STACK)" >&2
    exit 1
fi
echo "Verified stack limit bound: ${CURRENT_STACK} KiB (bounded stack reserve, not unconstrained)"

# Extract pinned historical pre-change blob
if git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    git -C "$REPO_ROOT" show "$PINNED_BLOB" > "$TMPDIR/sfo_base.c"
elif command -v git.exe >/dev/null 2>&1; then
    (cd "$REPO_ROOT" && git.exe show "$PINNED_BLOB") > "$TMPDIR/sfo_base.c"
else
    echo "ERROR: No working git command found to show pinned blob" >&2
    exit 1
fi

# Verify stack array signature is present in the pinned blob
if ! grep -F "sfo_kv_t kvs[KV_MAX_PAIRS];" "$TMPDIR/sfo_base.c" >/dev/null 2>&1; then
    echo "ERROR: Pinned blob does not contain historical stack arrays" >&2
    exit 1
fi

# Compile baseline tool
gcc -O2 -Wall -Wextra -Wno-stringop-truncation "$TMPDIR/sfo_base.c" -o "$TMPDIR/sfo_base"

# Generate golden fixtures under 8192 KiB bounded stack from neutral input.xml
"$TMPDIR/sfo_base" --fromxml "$SCRIPT_DIR/input.xml" "$SCRIPT_DIR/fixture.sfo"
"$TMPDIR/sfo_base" -l "$SCRIPT_DIR/fixture.sfo" > "$SCRIPT_DIR/fixture_list.txt"
"$TMPDIR/sfo_base" -t "$SCRIPT_DIR/fixture.sfo" "$SCRIPT_DIR/fixture_exported.xml"
"$TMPDIR/sfo_base" --fromxml "$SCRIPT_DIR/fixture_exported.xml" "$SCRIPT_DIR/fixture_roundtrip.sfo"

echo "Golden baseline fixtures generated successfully:"
sha256sum "$SCRIPT_DIR"/fixture*

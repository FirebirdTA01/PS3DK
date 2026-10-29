#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd -P)
exec python3 "$root/tests/host/heap-small-block-test.py" "$@"

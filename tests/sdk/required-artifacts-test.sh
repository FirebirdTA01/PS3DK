#!/usr/bin/env bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
exec python3 "$root/tests/sdk/required-artifacts-test.py" "$@"

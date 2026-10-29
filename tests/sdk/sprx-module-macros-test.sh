#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

py=$(command -v python3 || command -v python || true)
[ -n "$py" ] || { echo "sprx-module-macros: FAIL: no python"; exit 1; }
exec "$py" "${REPO_ROOT}/tests/sdk/sprx-module-macros-test.py" "$@"

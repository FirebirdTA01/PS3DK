#!/usr/bin/env bash
# Test tools/toc-midentry-audit.py on clean and crafted mid-entry fixtures.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$here/toc-midentry-audit-test.py" "$@"

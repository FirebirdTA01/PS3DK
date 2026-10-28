#!/usr/bin/env bash
# Test tools/toc-linked-check.py on synthetic links (host only; needs a readelf).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3; command -v $py > /dev/null 2>&1 || py=python
exec "$py" "$here/toc-linked-check-test.py" "$@"

#!/usr/bin/env bash
# Snapshot controls always run; SDK builds run when PS3DEV is available.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
exec "$py" -B "$here/samples-clean-build-test.py" "$@"

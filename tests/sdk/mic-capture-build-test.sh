#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
exec "$py" -B "$here/mic-capture-build-test.py" "$@"

#!/usr/bin/env bash
# Private host model: a native C compiler and Python 3, no installed SDK.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$here/auto-ring-model/run.py" "$@"

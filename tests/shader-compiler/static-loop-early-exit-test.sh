#!/usr/bin/env bash
# break and continue in a statically expanded loop, judged by value.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/static_loop_early_exit_check.py" "${1:?compiler required}"

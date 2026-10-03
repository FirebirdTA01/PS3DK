#!/usr/bin/env bash
# Returned helper paths leave their dead suffix unexecuted.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$here/inline_dead_tail_check.py" "${1:?compiler required}"

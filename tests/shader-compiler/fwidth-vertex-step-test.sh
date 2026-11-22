#!/usr/bin/env bash
# fwidth, and step in a vertex program; judged by twin, shape and value.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/fwidth_vertex_step_check.py" "${1:?compiler required}"

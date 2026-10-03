#!/usr/bin/env bash
# Selected-entry sampler input qualifiers preserve uniform texture bindings.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$here/entry_sampler_check.py" "${1:?compiler required}"

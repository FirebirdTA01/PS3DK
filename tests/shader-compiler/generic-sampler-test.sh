#!/usr/bin/env bash
# Generic samplers preserve distinct types and the measured 2D binding contract.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$here/generic_sampler_check.py" "${1:?compiler required}"

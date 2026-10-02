#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$here/vp_refract_check.py" "${1:?usage: vp-refract-test.sh compiler}"

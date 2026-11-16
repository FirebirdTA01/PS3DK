#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/a2p-probe-test.py" "$@"

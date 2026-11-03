#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/gcm-transfer-data-test.py" "$@"

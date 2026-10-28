#!/usr/bin/env bash
# Neutral-wording guard over the tracked tree and the change under test.
# See tests/host/neutral-wording-test.py.
set -euo pipefail
exec python3 -I -B "$(dirname "$0")/neutral-wording-test.py" "$@"

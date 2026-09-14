#!/usr/bin/env bash
# Test shader-compiler suite runner self-check (t_3d2a99cf).
# Verifies workflow parsing, outcome categorization (PASS/FAIL/TIMEOUT/UNRUNNABLE),
# SKIPPED section extraction, and execution timing arithmetic.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
python="${PYTHON:-python3}"
command -v "$python" >/dev/null 2>&1 || python="python"
command -v "$python" >/dev/null 2>&1 || { printf 'FAIL: python interpreter not found\n' >&2; exit 1; }

"$python" "$repo_root/tests/shader-compiler/suite-runner.py" --self-check
printf 'suite-runner-test: PASS\n'

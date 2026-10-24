#!/usr/bin/env bash
# ci/shellcheck.sh - the ShellCheck ratchet over scripts/**/*.sh (CI entry point).  The logic, and every fail-closed
# check, lives in ci/shellcheck_ratchet.py; see its docstring.  Pass --write-baseline to print a fresh baseline.
set -u
root=$(cd "$(dirname "$0")/.." && pwd) || { echo "FAIL: cannot resolve the repository root" >&2; exit 1; }
exec python3 "$root/ci/shellcheck_ratchet.py" --root "$root" "$@"

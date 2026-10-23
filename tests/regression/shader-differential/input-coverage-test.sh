#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$here/input-coverage-test.c" -o "$work/input-coverage-test"
"$work/input-coverage-test"

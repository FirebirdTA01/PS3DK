#!/usr/bin/env bash
# t_6be25fd4: a composed source must encode the same lanes as its direct form.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
python3 "$repo_root/tests/shader-compiler/local-swizzle-test.py" "$compiler"

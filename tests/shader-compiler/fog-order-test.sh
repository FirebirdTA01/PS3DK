#!/usr/bin/env bash
# Preserve sampled alpha and kill behavior while keeping fog work after KIL.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for name in fp_fog_after_kill_f fp_fog_distinct_inputs_f; do
    "$compiler" -p sce_fp_rsx --emit-container "$work/$name.fpo" \
        "$repo_root/tools/rsx-cg-compiler/tests/shaders/$name.cg"
done
python3 "$repo_root/tests/shader-compiler/fog_order_check.py" "$work/fp_fog_after_kill_f.fpo"
python3 "$repo_root/tests/shader-compiler/fog_order_check.py" "$work/fp_fog_distinct_inputs_f.fpo" --distinct
echo 'fog order: ok'

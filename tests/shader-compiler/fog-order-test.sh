#!/usr/bin/env bash
# Preserve sampled alpha and kill behavior while keeping fog work after KIL.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for name in fp_fog_after_kill_f fp_fog_distinct_inputs_f fp_discard_uncond_f; do
    "$compiler" -p sce_fp_rsx --emit-container "$work/$name.fpo" \
        "$repo_root/tools/rsx-cg-compiler/tests/shaders/$name.cg"
done
python3 "$repo_root/tests/shader-compiler/fog_order_check.py" "$work/fp_fog_after_kill_f.fpo"
python3 "$repo_root/tests/shader-compiler/fog_order_check.py" "$work/fp_fog_distinct_inputs_f.fpo" --distinct
python3 "$repo_root/tests/shader-compiler/fog_order_check.py" "$work/fp_discard_uncond_f.fpo" --unconditional
src="$repo_root/tools/rsx-cg-compiler/src"
"${CXX:-c++}" -std=c++17 -O0 -w -ffunction-sections -fdata-sections \
    -I"$src" -I"$src/nv40" -I"$src/donor" -I"$src/donor/ir" \
    -I"$src/donor/frontend" -I"$src/donor/common" \
    "$repo_root/tests/shader-compiler/kill-demand-test.cpp" \
    "$src/donor/ir/ir.cpp" -Wl,--gc-sections -o "$work/kill-demand"
"$work/kill-demand"
echo 'fog order: ok'

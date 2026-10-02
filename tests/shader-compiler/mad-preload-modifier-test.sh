#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
src="$repo_root/tools/rsx-cg-compiler/src"
"${CXX:-c++}" -std=c++17 -O0 -w -ffunction-sections -fdata-sections \
    -I"$src" -I"$src/nv40" -I"$src/donor" -I"$src/donor/ir" \
    -I"$src/donor/frontend" -I"$src/donor/common" \
    "$repo_root/tests/shader-compiler/mad-preload-modifier-test.cpp" \
    "$src/nv40/nv40_fp_assembler.cpp" "$src/donor/ir/ir.cpp" \
    -Wl,--gc-sections -o "$work/probe"
for kind in neg abs; do
    "$work/probe" "$kind" > "$work/$kind.words"
    "$compiler" -p sce_fp_rsx --emit-container "$work/fp_mad_shared_${kind}_f-ours.fpo" \
        "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_mad_shared_${kind}_f.cg"
done
args=()
if [[ -n "${MAD_MODIFIER_REFERENCE_DIR:-}" ]]; then
    args+=(--reference-directory "$MAD_MODIFIER_REFERENCE_DIR")
fi
python3 "$repo_root/tests/shader-compiler/mad_preload_modifier_check.py" "$work" "${args[@]}"
echo 'MAD preload modifiers: ok'

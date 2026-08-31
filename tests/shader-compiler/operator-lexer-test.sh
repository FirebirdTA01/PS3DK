#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-shader-modulo-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cat >"$work/mod_literal.fcg" <<'SHADER'
struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    int v = 7 % 4;
    o.color = float4((float)v, 0.0, 0.0, 1.0);
    return o;
}
SHADER

cat >"$work/mod_assign.fcg" <<'SHADER'
struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    int v = 7;
    v %= 4;
    o.color = float4((float)v, 0.0, 0.0, 1.0);
    return o;
}
SHADER

run_case() {
    local name="$1"
    local src="$work/$name.fcg"
    local out="$work/$name.bin"
    local log="$work/$name.log"
    local rc=0

    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering \
            --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    if [[ "$rc" -eq 124 ]]; then
        fail "$name timed out; modulo handling must be bounded"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "$name aborted or was killed under memory cap"
    fi
    if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
        fail "$name reported an allocation abort"
    fi

    if [[ "$rc" -ne 0 ]] && ! grep -q 'unsupported IR op mod' "$log"; then
        fail "$name failed without a clean modulo diagnostic"
    fi
}

run_case mod_literal
run_case mod_assign

unknown_cases=(
    "bitwise_and:int v = 7 & 4;:&:15"
    "bitwise_or:int v = 7 | 4;:|:15"
    "bitwise_xor:int v = 7 ^ 4;:^:15"
    "bitwise_not:int v = ~7;:~:13"
)

for item in "${unknown_cases[@]}"; do
    IFS=: read -r name expr char column <<<"$item"
    src="$work/$name.fcg"
    out="$work/$name.bin"
    log="$work/$name.log"
    cat >"$src" <<SHADER
struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    $expr
    o.color = float4((float)v, 0.0, 0.0, 1.0);
    return o;
}
SHADER

    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering \
            --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    [[ "$rc" -ne 0 ]] || fail "$name accepted an unsupported operator"
    if [[ "$rc" -eq 124 ]]; then
        fail "$name timed out; unknown-token handling must be bounded"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "$name aborted or was killed under memory cap"
    fi
    if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
        fail "$name reported an allocation abort"
    fi
    grep -Eq ":[0-9]+:$column: error: unknown character '\\$char'" "$log" \
        || fail "$name failed without a character+location lexer diagnostic"
done

printf 'operator-lexer-test: ok\n'

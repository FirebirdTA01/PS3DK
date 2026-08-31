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

cat >"$work/unused_bad_macro.fcg" <<'SHADER'
#define BAD ^

struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    o.color = float4(1.0, 0.0, 0.0, 1.0);
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

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --general-lowering \
        --emit-container "$work/unused_bad_macro.bin" "$work/unused_bad_macro.fcg"
) >"$work/unused_bad_macro.log" 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || fail "unused_bad_macro accepted an unknown token in a macro replacement list"
grep -Eq "unknown character '\\^'" "$work/unused_bad_macro.log" \
    || fail "unused_bad_macro failed without a named lexer diagnostic"
grep -Fq "$work/unused_bad_macro.fcg:" "$work/unused_bad_macro.log" \
    || fail "unused_bad_macro diagnostic did not name the source file"

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
    grep -Fq "$src:" "$log" \
        || fail "$name diagnostic did not name the source file"
    grep -Eq ":[0-9]+:$column: error: unknown character '\\$char'" "$log" \
        || fail "$name failed without a character+location lexer diagnostic"
done

cat >"$work/lexer_api_probe.cpp" <<'CPP'
#include "lexer.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

static int expect_error(const char* label, bool with_preprocessor)
{
    try
    {
        Lexer lexer("float4 value @\n", "lexer-api-probe.cg");
        std::vector<Token> tokens = with_preprocessor
            ? lexer.tokenizeWithPreprocessor()
            : lexer.tokenize();
        for (const Token& token : tokens)
        {
            if (token.type == TokenType::UNKNOWN)
            {
                std::cerr << label << " returned UNKNOWN instead of refusing it\n";
                return 1;
            }
        }
        std::cerr << label << " accepted an unknown character\n";
        return 1;
    }
    catch (const std::runtime_error& e)
    {
        const std::string msg = e.what();
        if (!contains(msg, "lexer-api-probe.cg:1:14") ||
            !contains(msg, "unknown character '@'"))
        {
            std::cerr << label << " diagnostic was not specific: " << msg << "\n";
            return 1;
        }
        return 0;
    }
}

int main()
{
    if (expect_error("tokenize", false) != 0)
        return 1;
    if (expect_error("tokenizeWithPreprocessor", true) != 0)
        return 1;
    return 0;
}
CPP

"${CXX:-c++}" -std=c++17 \
    -I"$repo_root/tools/rsx-cg-compiler/src/donor/frontend" \
    "$repo_root/tools/rsx-cg-compiler/src/donor/frontend/lexer.cpp" \
    "$work/lexer_api_probe.cpp" \
    -o "$work/lexer_api_probe"
"$work/lexer_api_probe"

printf 'operator-lexer-test: ok\n'

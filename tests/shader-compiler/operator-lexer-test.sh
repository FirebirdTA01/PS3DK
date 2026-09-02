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

cat >"$work/constant_bitwise_fp.fcg" <<'SHADER'
struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    int v = ((7 & 4) | (8 ^ 3)) + (~0) + (1 << 2) + (8 >> 1);
    o.color = float4(1.0, 0.0, 0.0, 1.0);
    return o;
}
SHADER

cat >"$work/constant_bitwise_vp.vcg" <<'SHADER'
float4 main(float4 pos : POSITION) : POSITION
{
    int v = ((7 & 4) | (8 ^ 3)) + (~0) + (1 << 2) + (8 >> 1);
    return pos;
}
SHADER

cat >"$work/live_constant_bitwise_fp.fcg" <<'SHADER'
struct OUT { float4 color : COLOR0; };

OUT main()
{
    OUT o;
    bool b = (3 & 1) == 1;
    o.color = b ? float4(1.0, 0.0, 0.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
    return o;
}
SHADER

cat >"$work/live_constant_bitwise_vp.vcg" <<'SHADER'
float4 main(float4 pos : POSITION) : POSITION
{
    bool b = (3 & 1) == 1;
    return b ? pos : float4(0.0, 0.0, 0.0, 1.0);
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

    # The '%' token must still REACH the IR.  The defect this test was written
    # for was the lexer silently DROPPING unknown tokens, and a dropped '%'
    # leaves the parser looking at a malformed expression - so a parse-level
    # error is the regression, not a backend refusal.
    if grep -Eqi 'unexpected token|parse error|syntax error' "$log"; then
        fail "$name failed in the parser; the '%' token may have been dropped"
    fi

    # A nonzero exit is legitimate: this shader also uses conversions the
    # backend may not implement yet, and refusing is the house rule.  What
    # matters is that the refusal is NAMED rather than silent.
    #
    # This deliberately does NOT pin a single op name.  It used to require
    # 'unsupported IR op mod', which went stale the moment mod was implemented
    # (slice B) - the compiler then refused correctly one op further down, on
    # itof, and this test turned CI red for a compiler that was behaving
    # exactly as intended.  Pinning the frontier op means every feature that
    # lands breaks the test that guards the layer beneath it.
    if [[ "$rc" -ne 0 ]] && ! grep -Eq 'nv40-(fp|vp|general):' "$log"; then
        fail "$name failed without a named diagnostic"
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
[[ "$rc" -eq 0 ]] || { tail -n 10 "$work/unused_bad_macro.log" >&2; fail "unused_bad_macro refused an unused bitwise token in a macro replacement list"; }
[[ -s "$work/unused_bad_macro.bin" ]] || fail "unused_bad_macro produced no container"
if grep -Eq "unknown character|not supported by this profile" "$work/unused_bad_macro.log"; then
    tail -n 10 "$work/unused_bad_macro.log" >&2
    fail "unused_bad_macro treated an unused macro replacement token as active code"
fi

for profile in sce_fp_rsx sce_vp_rsx; do
    src="$work/constant_bitwise_fp.fcg"
    [[ "$profile" == sce_vp_rsx ]] && src="$work/constant_bitwise_vp.vcg"
    for mode in default general; do
        args=()
        [[ "$mode" == general ]] && args+=(--general-lowering)
        rc=0
        (
            ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
            timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
                -p "$profile" "${args[@]}" \
                --emit-container "$work/constant_bitwise_${profile}_${mode}.bin" "$src"
        ) >"$work/constant_bitwise_${profile}_${mode}.log" 2>&1 || rc=$?
        [[ "$rc" -eq 0 ]] || { tail -n 10 "$work/constant_bitwise_${profile}_${mode}.log" >&2; fail "constant_bitwise refused on $profile/$mode"; }
        [[ -s "$work/constant_bitwise_${profile}_${mode}.bin" ]] || fail "constant_bitwise produced no container on $profile/$mode"
        if grep -Eq "unknown character|not supported by this profile|unsupported IR op" "$work/constant_bitwise_${profile}_${mode}.log"; then
            tail -n 10 "$work/constant_bitwise_${profile}_${mode}.log" >&2
            fail "constant_bitwise did not fold before the profile/backend checks on $profile/$mode"
        fi
    done
done

for profile in sce_fp_rsx sce_vp_rsx; do
    src="$work/live_constant_bitwise_fp.fcg"
    [[ "$profile" == sce_vp_rsx ]] && src="$work/live_constant_bitwise_vp.vcg"
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" --general-lowering \
            --emit-container "$work/live_constant_bitwise_$profile.bin" "$src"
    ) >"$work/live_constant_bitwise_$profile.log" 2>&1 || rc=$?
    [[ "$rc" -eq 0 ]] || { tail -n 10 "$work/live_constant_bitwise_$profile.log" >&2; fail "live_constant_bitwise refused on $profile/general"; }
    [[ -s "$work/live_constant_bitwise_$profile.bin" ]] || fail "live_constant_bitwise produced no container on $profile/general"
    if grep -Eq "unknown character|not supported by this profile|unsupported IR op" "$work/live_constant_bitwise_$profile.log"; then
        tail -n 10 "$work/live_constant_bitwise_$profile.log" >&2
        fail "live_constant_bitwise did not fold before the profile/backend checks on $profile/general"
    fi
done

profile_cases=(
    "bitwise_and:bool b = (u & 4) != 0;:&:20"
    "bitwise_or:bool b = (u | 4) != 0;:|:20"
    "bitwise_xor:bool b = (u ^ 4) != 0;:^:20"
    "bitwise_not:bool b = (~u) != 0;:~:19"
    "shift_left:bool b = (u << 1) != 0;:<<:20"
    "shift_right:bool b = (u >> 1) != 0;:>>:20"
)

for item in "${profile_cases[@]}"; do
    IFS=: read -r name expr char column <<<"$item"
    fp_src="$work/${name}_fp.fcg"
    cat >"$fp_src" <<SHADER
struct OUT { float4 color : COLOR0; };

OUT main(uniform int u)
{
    OUT o;
    $expr
    o.color = b ? float4(1.0, 0.0, 0.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
    return o;
}
SHADER
    vp_src="$work/${name}_vp.vcg"
    cat >"$vp_src" <<SHADER
float4 main(float4 pos : POSITION, uniform int u) : POSITION
{
    $expr
    return b ? pos : float4(pos.x, pos.y, pos.z, 1.0);
}
SHADER

    for profile in sce_fp_rsx sce_vp_rsx; do
        src="$fp_src"
        [[ "$profile" == sce_vp_rsx ]] && src="$vp_src"
        for mode in default general; do
            args=()
            [[ "$mode" == general ]] && args+=(--general-lowering)
            out="$work/${name}_${profile}_${mode}.bin"
            log="$work/${name}_${profile}_${mode}.log"
            rc=0
            (
                ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
                timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
                    -p "$profile" "${args[@]}" \
                    --emit-container "$out" "$src"
            ) >"$log" 2>&1 || rc=$?

            [[ "$rc" -ne 0 ]] || fail "$name accepted an unsupported operator on $profile/$mode"
            if [[ "$rc" -eq 124 ]]; then
                fail "$name timed out on $profile/$mode; unsupported-op handling must be bounded"
            fi
            if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
                fail "$name aborted or was killed under memory cap on $profile/$mode"
            fi
            if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
                fail "$name reported an allocation abort on $profile/$mode"
            fi
            grep -Fq "$src:" "$log" \
                || fail "$name diagnostic did not name the source file on $profile/$mode"
            if grep -Eq "unknown character|unexpected token|parse error|syntax error|unsupported IR op" "$log"; then
                tail -n 10 "$log" >&2
                fail "$name failed before the profile check on $profile/$mode"
            fi
            grep -Fq "C5508: the operator \"$char\" is not supported by this profile" "$log" \
                || fail "$name failed without the profile-level bitwise diagnostic on $profile/$mode"
        done
    done
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

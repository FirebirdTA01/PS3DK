#!/usr/bin/env bash
# t_a90b1ef1: every declarator in one declaration is declared.
#
# `float a, b;` declared only `a`.  Using `b` afterwards was then refused as
# an UNDECLARED IDENTIFIER - a variable the shader declares, reported as one
# it did not.  Every declarator after the first was dropped, at file scope
# and inside a function, with and without initialisers.
#
# The oracle is unambiguous here, unlike the local-type diagnostic row: the
# reference ACCEPTS all four shapes below and rejects the genuinely
# undeclared control, so this test compares our own comma form against our
# own separate-statement form and requires them to agree, with the reference
# having settled which behaviour is correct.
#
# The pairs are byte-compared rather than merely both-compile: a declaration
# that is accepted but binds the wrong storage would pass an acceptance-only
# check.  Two shaders agreeing is not two shaders being right, so the
# separate-statement form is the one the reference and we already agree on.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-multi-declarator-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

emit() {  # <stem> <source text>; echoes the exit status
    local stem="$1" text="$2"
    printf '%s' "$text" > "$work/$stem.cg"
    rm -f "$work/$stem.fpo"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$work/$stem.cg"
    ) >"$work/$stem.log" 2>&1
    local status=$?
    set -e
    printf '%s' "$status"
}

# A comma form and the separate-statement form of the same program.  The
# separate form is what both compilers already agree on, so it is the
# reference point the comma form has to meet.
pair() {  # <name> <comma source> <separate source>
    local name="$1" comma="$2" separate="$3"
    local cs ss
    cs="$(emit "${name}_comma" "$comma")"
    ss="$(emit "${name}_separate" "$separate")"

    [[ "$ss" == 0 ]] || {
        tail -n 3 "$work/${name}_separate.log" >&2
        fail "$name: the SEPARATE-statement control did not compile (exit $ss); the fixture is wrong, not the compiler"
    }
    [[ "$cs" == 0 ]] || {
        tail -n 3 "$work/${name}_comma.log" >&2
        fail "$name: the comma form exited $cs - a declarator after the first is being dropped, so a declared name reads as undeclared"
    }
    cmp -s "$work/${name}_comma.fpo" "$work/${name}_separate.fpo" || fail \
        "$name: the comma form compiled but produced different bytes from the separate-statement form - it is accepted and bound differently, which an acceptance-only check would have passed"
    printf '  %-22s ok\n' "$name"
}

pair two_locals \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a, b;
    a = c.x;
    b = c.y;
    o = float4(a, b, 0, 1);
}
' \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a;
    float b;
    a = c.x;
    b = c.y;
    o = float4(a, b, 0, 1);
}
'

pair with_initialisers \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a = c.x, b = c.y;
    o = float4(a, b, 0, 1);
}
' \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a = c.x;
    float b = c.y;
    o = float4(a, b, 0, 1);
}
'

pair three_declarators \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a, b, d;
    a = c.x;
    b = c.y;
    d = c.z;
    o = float4(a, b, d, 1);
}
' \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a;
    float b;
    float d;
    a = c.x;
    b = c.y;
    d = c.z;
    o = float4(a, b, d, 1);
}
'

pair file_scope \
'float g1, g2;
void main(out float4 o : COLOR)
{
    o = float4(g1, g2, 0, 1);
}
' \
'float g1;
float g2;
void main(out float4 o : COLOR)
{
    o = float4(g1, g2, 0, 1);
}
'

pair vector_type \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float3 u, v;
    u = c.xyz;
    v = c.yzw;
    o = float4(u.x + v.x, u.y, v.z, 1);
}
' \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float3 u;
    float3 v;
    u = c.xyz;
    v = c.yzw;
    o = float4(u.x + v.x, u.y, v.z, 1);
}
'

# THE CONTROL THAT MUST STILL REFUSE.  A fix that declares names it should
# not - for instance by treating any identifier after a comma as declared -
# would satisfy every pair above and break this.
undeclared_status="$(emit undeclared \
'void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a;
    a = c.x;
    o = float4(a, notDeclared, 0, 1);
}
')"
[[ "$undeclared_status" == 1 ]] || fail \
    "the undeclared control exited $undeclared_status, expected exactly 1 - a genuinely undeclared name must still be refused"
[[ -f "$work/undeclared.fpo" ]] && fail "the undeclared control refused but still wrote a container"
grep -qF "undeclared identifier 'notDeclared'" "$work/undeclared.log" || {
    head -n 3 "$work/undeclared.log" >&2
    fail "the undeclared control refused without naming notDeclared - an unrelated failure would otherwise satisfy this control"
}
printf '  %-22s ok\n' "undeclared refused"

# DUPLICATE DECLARATORS MUST REFUSE.  The row has two halves and this is the
# dangerous one: `float a, a;` compiled and WROTE A CONTAINER, so we accepted
# invalid source as well as rejecting valid names.  The reference refuses
# both spellings by name, so a fix that simply declares every declarator
# without checking for a repeat would trade one defect for a quieter one.
for scope in local global; do
    if [[ "$scope" == local ]]; then
        dup_src='void main(float4 c : TEXCOORD0, out float4 o : COLOR)
{
    float a, a;
    a = c.x;
    o = float4(a, 0, 0, 1);
}
'
        dup_name=a
    else
        dup_src='float g, g;
void main(out float4 o : COLOR)
{
    o = float4(g, 0, 0, 1);
}
'
        dup_name=g
    fi
    dup_status="$(emit "dup_$scope" "$dup_src")"
    [[ "$dup_status" == 1 ]] || fail \
        "duplicate declarator ($scope) exited $dup_status, expected exactly 1 - the reference refuses it as already defined, and we wrote a container for it"
    [[ -f "$work/dup_$scope.fpo" ]] && fail \
        "duplicate declarator ($scope) refused but still wrote a container"
    # The BODY of the diagnostic, not the identifier anywhere in the log:
    # `a` and `g` are single letters that appear in almost any output,
    # including a path, so a bare grep for one is satisfied by a refusal
    # for a completely unrelated reason.  Pin the rule and the name.
    grep -qF "redefinition of variable '$dup_name'" "$work/dup_$scope.log" || {
        head -n 3 "$work/dup_$scope.log" >&2
        fail "duplicate declarator ($scope) refused without the redefinition diagnostic naming '$dup_name'; an unrelated failure would otherwise satisfy this control"
    }
    printf '  %-22s ok\n' "duplicate $scope refused"
done

printf 'multi-declarator-test: ok (five comma/separate pairs byte-identical; a genuinely undeclared name and both duplicate declarators still refused)\n'
printf 'PASS: multi-declarator-test\n'

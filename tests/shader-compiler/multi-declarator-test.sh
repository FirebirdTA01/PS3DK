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
# invalid source as well as rejecting valid names.  The reference refuses both
# spellings by name, so a fix that declares every declarator without checking
# for a repeat would trade one defect for a quieter one.
#
# The check takes a COMPILER so the self-check below can point it at a stub and
# watch it reject one.  It pins the diagnostic BODY - the rule and the
# duplicated name - because the names here are single letters: a bare search
# for `a` is satisfied by a path, a temp file name, or any unrelated message,
# and a refusal for the wrong reason then reads as a pass.  That exact hole was
# found in this guard by a wrapper refusing with an unrelated diagnostic.
duplicate_source() {  # <scope>
    if [[ "$1" == local ]]; then
        printf 'void main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    float a, a;\n    a = c.x;\n    o = float4(a, 0, 0, 1);\n}\n'
    else
        printf 'float g, g;\nvoid main(out float4 o : COLOR)\n{\n    o = float4(g, 0, 0, 1);\n}\n'
    fi
}

check_duplicate() {  # <compiler> <scope> <tag>; echoes a problem, or nothing
    local cc="$1" scope="$2" tag="$3" dir src out log dup_name status
    dup_name=a; [[ "$scope" == local ]] || dup_name=g
    # The BASENAMES stay dup_local.cg / dup_global.cg and the run is
    # separated by directory instead.  A reviewer's wrapper keys on the
    # source name to target exactly these two cases; renaming them to
    # dup_real_local.cg silently turned such a wrapper into a
    # pass-through, and the guard then looked stronger than it was.
    dir="$work/$tag"
    mkdir -p "$dir"
    src="$dir/dup_$scope.cg"; out="$dir/dup_$scope.fpo"; log="$dir/dup_$scope.log"
    duplicate_source "$scope" > "$src"
    rm -f "$out"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>&1
    status=$?
    set -e
    if [[ "$status" != 1 ]]; then
        printf 'exited %s, expected exactly 1' "$status"
        return 0
    fi
    if [[ -f "$out" ]]; then
        printf 'refused but still wrote a container'
        return 0
    fi
    if ! grep -qF "redefinition of variable '$dup_name'" "$log"; then
        printf 'no redefinition diagnostic naming %s' "$dup_name"
        return 0
    fi
}

for scope in local global; do
    problem="$(check_duplicate "$compiler" "$scope" real)"
    [[ -z "$problem" ]] || fail "duplicate declarator ($scope): $problem"
    printf '  %-22s ok\n' "duplicate $scope refused"
done

# SELF-CHECK, through the SAME function the real results go through: a stub
# that refuses properly - exit 1, no container - but for an UNRELATED reason
# must be rejected, independently for each scope so neither arm can hide
# behind the other failing first.  Before the diagnostic body was pinned this
# shape passed the whole guard, and the only evidence it now fails lived in a
# reviewer's private wrapper; this keeps it.
stub="$work/unrelated-refusal"
{
    echo '#!/usr/bin/env bash'
    echo 'echo "stub.cg:1:1: error: unrelated stage failure" >&2'
    echo 'exit 1'
} > "$stub"
chmod +x "$stub"
for scope in local global; do
    stub_problem="$(check_duplicate "$stub" "$scope" stub)"
    [[ -n "$stub_problem" ]] || fail \
        "a stub refusing with an unrelated diagnostic satisfied the duplicate control ($scope) - the assertion is not reading the diagnostic body"
    case "$stub_problem" in
        *"no redefinition diagnostic"*) ;;
        *) fail "the unrelated-refusal stub was rejected for the wrong reason ($scope): $stub_problem" ;;
    esac
    printf '  %-22s ok\n' "unrelated $scope rejected"
done

printf 'multi-declarator-test: ok (five comma/separate pairs byte-identical; a genuinely undeclared name and both duplicate declarators still refused)\n'
printf 'PASS: multi-declarator-test\n'

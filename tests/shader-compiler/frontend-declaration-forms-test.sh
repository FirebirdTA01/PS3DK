#!/usr/bin/env bash
# Three declaration/preprocessor forms the reference SDK's own headers use
# and our frontend rejected, each measured against the reference before it
# was changed.
#
# 1. A PREPROCESSOR DIRECTIVE NAME ENDS AT THE FIRST NON-IDENTIFIER
#    CHARACTER, not at the first space.  `#if(SAMPLE_COUNT > 1)` is the
#    directive `if` whose expression starts with a parenthesis; we read the
#    whole whitespace-delimited token as the name and reported an unknown
#    directive.  The reference's behaviour, measured on four forms:
#
#      #if(N > 1)    accepted     #elif(N > 1)  accepted
#      #endif(N)     accepted     #ifdef(N)     REFUSED, C0105
#                                               "Syntax error in #ifdef"
#
#    The last one is why the rule is "parse the name, then let the
#    directive judge its own argument" and not "accept anything after a
#    known prefix": the reference RECOGNISES `ifdef` there - it reports a
#    syntax error IN it, not the C0104 "Unknown pre-processor directive" it
#    gives for `#frobnicate`.  Both of those distinctions are asserted
#    below, because a fix that accepted `#ifdef(N)` would also pass a test
#    that only checked `#if(`.
#
# 2. `typedef struct { ... } Name;` - an ANONYMOUS struct named by the
#    typedef.  DeferredShading/shaders/light_structs.cgh declares every one
#    of its varying structs this way.  parseType() cannot see a struct
#    BODY, so this failed at the brace.
#
# 3. A STORAGE QUALIFIER ON A STRUCT MEMBER - `uniform sampler2D t :
#    TEXUNIT0;` and `in float2 uv : TEXCOORD0;`.  The member's type was
#    never reached and the parser reported "Expected type name" AT THE
#    QUALIFIER.  The qualifier is recorded on the field rather than
#    skipped: nothing reads it yet, because the semantic is what binds the
#    member, but a qualifier parsed and thrown away cannot be told from one
#    that was never written.
#
# What this does NOT claim: that the shaders these forms come from now
# compile.  Clearing a first blocker moves a source to its next one, and
# these three move the DeferredShading sources onto pack_2half/
# unpack_4ubyte, which are a separate gap.  The row that this closes end to
# end is the head_tracker family (11 of 12 sources), and the fixtures here
# are the forms, not those sources.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# A refusal is exit 1 EXACTLY.  124 is a timeout and >=128 is a signal, and
# either satisfies "did not exit 0" while meaning the compiler never reached
# the decision this guard is about (t_fd95d1b9).
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-frontend-decl-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run() {   # $1 stem -> sets rc, writes $work/$1.log and $work/$1.fpo
    local stem="$1"
    rm -f "$work/$stem.fpo"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$work/$stem.cg"
    ) >"$work/$stem.log" 2>&1
    rc=$?
    set -e
}

accepts() {   # $1 stem, $2 what it is
    run "$1"
    refusal_status "$rc" "$1"
    [[ "$rc" -eq 0 ]] || {
        head -n 4 "$work/$1.log" >&2
        fail "$2 did not compile; the reference accepts it"
    }
    [[ -s "$work/$1.fpo" ]] || fail "$2 exited 0 but wrote no container"
}

refuses() {   # $1 stem, $2 what it is, $3 text the diagnostic must contain
    run "$1"
    refusal_status "$rc" "$1"
    [[ "$rc" -eq 1 ]] || fail "$2 compiled; the reference refuses it"
    [[ -f "$work/$1.fpo" ]] && fail "$2 refused but still wrote a container"
    grep -qi -- "$3" "$work/$1.log" || {
        head -n 4 "$work/$1.log" >&2
        fail "$2 refused without naming '$3'; a refusal that does not say which rule made it is a dead end"
    }
    return 0
}

# --- 1. directive names -----------------------------------------------
cat >"$work/if_paren.cg" <<'CG'
#define N 2
#if(N > 1)
float4 main() : COLOR { return float4(1,0,0,1); }
#else
float4 main() : COLOR { return float4(0,1,0,1); }
#endif
CG
cat >"$work/elif_paren.cg" <<'CG'
#define N 2
#if(N > 5)
float4 main() : COLOR { return float4(1,0,0,1); }
#elif(N > 1)
float4 main() : COLOR { return float4(0,1,0,1); }
#endif
CG
cat >"$work/endif_junk.cg" <<'CG'
#define N 2
#if(N > 1)
float4 main() : COLOR { return float4(1,0,0,1); }
#endif(N)
CG
cat >"$work/ifdef_paren.cg" <<'CG'
#define N 2
#ifdef(N)
float4 main() : COLOR { return float4(1,0,0,1); }
#endif
CG
cat >"$work/unknown_directive.cg" <<'CG'
#frobnicate 1
float4 main() : COLOR { return float4(1,0,0,1); }
CG

accepts if_paren   '#if( with no space'
accepts elif_paren '#elif( with no space'
accepts endif_junk '#endif with trailing text'

# The two refusals must be DIFFERENT refusals.  `#ifdef(N)` is a bad
# ARGUMENT to a directive we recognise; `#frobnicate` is an unknown
# directive.  A fix that lumped them together would pass a test that only
# checked "both refuse".
refuses ifdef_paren       '#ifdef( with no space' 'ifdef'
grep -qi 'unknown' "$work/ifdef_paren.log" && \
    fail "#ifdef(N) was reported as an UNKNOWN directive; the reference recognises ifdef there and reports a syntax error in it (C0105), so the name was parsed and only its argument is wrong"
refuses unknown_directive 'an unknown directive' 'frobnicate'

# --- 2. anonymous struct typedef --------------------------------------
cat >"$work/anon_typedef.cg" <<'CG'
typedef struct {
	float2 a : TEXCOORD0;
	float2 b : TEXCOORD1;
} vin_t;

float4 main(vin_t i) : COLOR { return float4(i.a, i.b); }
CG
accepts anon_typedef 'typedef struct { ... } Name;'

# CONTROL: the named spelling must still work, so a fix that rerouted every
# struct through the typedef path would fail here rather than pass quietly.
cat >"$work/named_struct.cg" <<'CG'
struct vin_t {
	float2 a : TEXCOORD0;
	float2 b : TEXCOORD1;
};

float4 main(vin_t i) : COLOR { return float4(i.a, i.b); }
CG
accepts named_struct 'struct Name { ... };'

# --- 3. storage qualifiers on struct members --------------------------
cat >"$work/member_qualifier.cg" <<'CG'
typedef struct {
	in float2 uv : TEXCOORD0;
	in float4 col : COLOR0;
} vin_t;

float4 main(vin_t i) : COLOR { return i.col * float4(i.uv, 0, 1); }
CG
accepts member_qualifier 'in-qualified struct members'

printf 'frontend-declaration-forms-test: PASS\n'

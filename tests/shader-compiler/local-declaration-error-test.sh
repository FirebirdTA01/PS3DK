#!/usr/bin/env bash
# t_ec186f0c: one mistake, one message, at the mistake.
#
# `bogusType q;` inside a function body was parsed as an EXPRESSION
# statement, because the declaration path is only taken when the first token
# is a known type name.  The result was five diagnostics for one mistake, the
# first pointing at the identifier AFTER the unknown type and a later one
# inventing `unknown type name 'o'` about a name that is declared - so a
# reader is sent to the wrong line about the wrong identifier.
#
# THERE IS NO ORACLE FOR THIS ONE, and that is a deliberate, recorded
# decision rather than an oversight.  The reference cascades on this input
# too - five messages, including a "profile specifier" warning and an error
# on the following line - so matching it would mean copying a cascade it did
# not intend.  The rule the project settled on: the oracle governs EMITTED
# CODE, a diagnostic is not part of that contract, and where the reference
# emits a cascade with no intended single answer we may be strictly better
# provided we say we are choosing it.  Other guards here carry expectations
# of their own - the parse-progress rule and the preprocessor harness among
# them; what this one adds is saying so, in the place a reader checking the
# expectation will look.
#
# The contract, which is the same one the PARAMETER path already ships:
# report `unknown type name '<name>'` at the unknown type's own line and
# column, consume to the ';', and keep parsing - so a second, real mistake
# later in the body is still reported.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-local-declaration-error-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
fixture="$shaders/fp_unknown_type_local_f.cg"
[[ -f "$fixture" ]] || fail "fixture missing: $fixture"

# Where the mistake actually is, read from the fixture rather than written
# down, so the expectation cannot drift from the file it describes.
want_line="$(grep -n 'bogusType' "$fixture" | grep -v '//' | head -1 | cut -d: -f1)"
want_col="$(awk '/bogusType/ && $0 !~ /^[[:space:]]*\/\// { print index($0, "bogusType"); exit }' "$fixture")"
[[ -n "$want_line" && -n "$want_col" ]] || fail "could not locate bogusType in $fixture"

run() {  # <source> <out>; echoes the exit status
    local src="$1" out="$2"
    rm -f "$out"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$out.log" 2>&1
    local status=$?
    set -e
    printf '%s' "$status"
}

# ---- one mistake ----------------------------------------------------------
out="$work/single.fpo"
status="$(run "$fixture" "$out")"
[[ "$status" == 1 ]] || {
    tail -n 5 "$out.log" >&2
    fail "the fixture exited $status, expected exactly 1 (124 is a timeout and a signal is a crash; neither is a refusal)"
}
[[ -f "$out" ]] && fail "the fixture refused but still wrote a container"

errors="$(grep -cE ': error: ' "$out.log" || true)"
[[ "$errors" == 1 ]] || {
    grep -E ': error: ' "$out.log" >&2
    fail "one mistake produced $errors diagnostics; the contract for this shape is exactly one"
}

# Anchored on the BASENAME rather than the full path: Git Bash rewrites a
# POSIX argv path into Windows form before exec'ing a native binary, so the
# compiler echoes 'C:/Users/FIREBI~1/...' for a '/c/Users/...' argument and
# the directory it prints is the shell's spelling, not a compiler decision.
# The line, the column and the message - what this guard is actually for -
# are all still pinned.
expected="$(basename "$fixture"):$want_line:$want_col: error: unknown type name 'bogusType'"
grep -qF "$expected" "$out.log" || {
    head -n 3 "$out.log" >&2
    fail "expected <$expected>; the diagnostic must name the unknown TYPE at its own line and column, not the identifier after it"
}

# ---- two mistakes: recovery continues -------------------------------------
# Consuming to the ';' has to leave the parser able to report the NEXT real
# mistake.  A fix that stopped at the first one would satisfy the count above
# and quietly hide everything after it.
control="$work/two_mistakes.cg"
printf 'void main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    otherBogus r;\n    o = c;\n}\n' > "$control"
control_out="$work/two.fpo"
status="$(run "$control" "$control_out")"
[[ "$status" == 1 ]] || {
    tail -n 5 "$control_out.log" >&2
    fail "the two-mistake control exited $status, expected exactly 1"
}
for name in bogusType otherBogus; do
    grep -qF "error: unknown type name '$name'" "$control_out.log" || {
        grep -E ': error: ' "$control_out.log" >&2
        fail "the two-mistake control did not report '$name'; recovery must continue past the first unknown type, or a fix hides every mistake after the first"
    }
done
[[ -f "$control_out" ]] && fail \
    "the two-mistake control refused but still wrote a container; a fix that recovers must still refuse the program"
control_errors="$(grep -cE ': error: ' "$control_out.log" || true)"
[[ "$control_errors" == 2 ]] || {
    grep -E ': error: ' "$control_out.log" >&2
    fail "two mistakes produced $control_errors diagnostics; expected exactly 2"
}

# ---- the control that must not move ---------------------------------------
# A valid program with a local declaration of a KNOWN type still compiles:
# the new path must recognise a declaration attempt, not swallow declarations.
good="$work/good.cg"
printf 'void main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    float4 q = c;\n    o = q;\n}\n' > "$good"
good_out="$work/good.fpo"
status="$(run "$good" "$good_out")"
[[ "$status" == 0 ]] || {
    tail -n 5 "$good_out.log" >&2
    fail "a local declaration of a known type stopped compiling (exit $status)"
}
[[ -s "$good_out" ]] || fail "the valid control wrote no container"

printf 'local-declaration-error-test: ok (one mistake reported once at its own line and column, two mistakes both reported, a valid local declaration still compiles)\n'
printf 'PASS: local-declaration-error-test\n'

#!/usr/bin/env bash
# t_e5fced4c: the preprocessor properties no command line can reach.
#
# Three defects shipped through this blind spot and none could have been
# caught by a test that drives the compiler through argv:
#   - setNoLineMarkers(true) was bypassed by markers generated at a line
#     discontinuity.  main.cpp never calls that setter, so the option is
#     unreachable from the command line.
#   - an #include left __FILE__ naming the child after it returned.  __FILE__
#     expands to a string the shader language rejects, and the diagnostic
#     that follows quotes nothing, so the VALUE is invisible from outside.
#   - MacroDefinition::isVariadic was read uninitialised by the expander for
#     EVERY function-like macro.  No container comparison could see it; a
#     sanitizer run on a function-like expansion is what found it.
# All three were found with a driver that calls Preprocessor::process
# directly.  This is that driver, made permanent.
#
# The expectations are EXACT STDOUT, compared as bytes against a file - not
# through a command substitution, which strips trailing newlines and would
# make "exact" a claim this test did not check. These fixtures are deliberately
# tiny: anything that changes their output is worth a person
# reading it, including a change that looks harmless.
#
# THE SANITIZED PASS MUST EXPAND A FUNCTION-LIKE MACRO.  Object-like X and
# __FILE__ never reach the code that read the uninitialised bool, so a
# sanitized run over those alone would pass while the defect this row exists
# for went unexercised.
# Marker-relative __FILE__ remains a separate open property (t_b14c4cf7).
# These fixtures assert physical include filenames, not a #line rename.
# Include restoration is checked on normal return only; restoration after an
# included file throws is not exercised. Exact paths target the Linux CI job:
# Git Bash rewriting argv for a native Windows probe would need path handling.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src_dir="$repo_root/tools/rsx-cg-compiler/src/donor/frontend"
driver="$repo_root/tests/shader-compiler/harness/preprocessor_probe.cpp"
[[ -f "$driver" ]] || fail "harness driver missing: $driver"

cxx="${CXX:-g++}"
command -v "$cxx" >/dev/null 2>&1 || fail \
    "no C++ compiler ($cxx): this guard reaches the preprocessor API directly and cannot fall back to the command line, so it fails rather than reporting a green it did not earn"
command -v timeout >/dev/null 2>&1 || fail "timeout is required to bound each harness invocation"
case_timeout=10s

work="${TMPDIR:-/tmp}/ps3dk-preprocessor-harness-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

build() {  # <output> <extra flags...>
    local out="$1"; shift
    timeout --kill-after=5s 180s "$cxx" -std=c++17 -O0 -g -I "$src_dir" "$@" \
        -o "$out" "$driver" "$src_dir/preprocessor.cpp" "$src_dir/lexer.cpp" \
        >"$out.build.log" 2>&1 || {
        tail -n 20 "$out.build.log" >&2
        fail "could not build the harness ($*)"
    }
}

build "$work/probe"
build "$work/probe-ubsan" -O1 -ftrivial-auto-var-init=pattern \
    -fsanitize=undefined -fno-sanitize-recover=all

# Fixtures.  Generated, because a .cg under tests/shaders is compiled by the
# tree sweeps and these are not shaders.
printf '#define X 1\nX\n' > "$work/markers.cg"
printf '__FILE__\n#include "child.h"\n__FILE__\n' > "$work/file_restore.cg"
printf '__FILE__\n' > "$work/child.h"
printf '#define ADD(a, b) ((a) + (b))\nADD(1, 2)\n' > "$work/funclike.cg"
printf '#define ONE(x) (x)\nONE(1, 2)\n' > "$work/arity.cg"
printf '#define ONE(x) x\n#define DROP(x) 1.0\nDROP(ONE(1,2))\n' > "$work/unused_arity.cg"
printf '__FILE__\n#include "nested.h"\n__FILE__\nRETAINED\n' > "$work/nested.cg"
printf '__FILE__\n#include "leaf.h"\n__FILE__\n' > "$work/nested.h"
printf '__FILE__\n#define RETAINED 7\n' > "$work/leaf.h"
: > "$work/empty.cg"

# Expected stdout, byte for byte.
printf '#line 2 "%s"\n1\n' "$work/markers.cg" > "$work/markers_on.expected"
printf '1\n' > "$work/markers_off.expected"
printf '"%s"\n"%s"\n"%s"\n' \
    "$work/file_restore.cg" "$work/child.h" "$work/file_restore.cg" \
    > "$work/file_restore.expected"
printf '((1) + (2))\n' > "$work/funclike.expected"
printf '"%s"\n"%s"\n"%s"\n"%s"\n"%s"\n7\n' \
    "$work/nested.cg" "$work/nested.h" "$work/leaf.h" \
    "$work/nested.h" "$work/nested.cg" > "$work/nested.expected"
printf 'preprocessor threw: Macro ONE called with incorrect number of arguments\n' \
    > "$work/arity.err.expected"
printf 'cannot open %s\n' "$work/does-not-exist.cg" > "$work/missing.err.expected"
printf '%s is empty\n' "$work/empty.cg" > "$work/empty.err.expected"

# The driver's own exit contract, pinned as its own numbers rather than
# renumbered to imitate the compiler: 0 success, 3 the input could not be
# opened or was empty, 4 the preprocessor threw.  A usage error is 2.  The
# point of pinning them is to reject a timeout, a crash or an unrelated
# failure - not to make every API driver speak the CLI's exit code.
readonly EXIT_OK=0
readonly EXIT_INPUT=3
readonly EXIT_THREW=4

# Runs one case and reports what is wrong with it, or nothing.  Returning a
# description rather than exiting is what lets the negatives below check that
# these assertions actually reject something.
check_case() {  # <probe> <expected-status> <expected-stdout-file|-> <expected-stderr-file|-> <args...>
    local probe="$1" want_status="$2" expected="$3" expected_err="$4"; shift 4
    local status
    set +e
    timeout --kill-after=1s "$case_timeout" "$probe" "$@" >"$work/case.out" 2>"$work/case.err"
    status=$?
    set -e
    # Check EVERY invocation, before another case overwrites its log. A
    # recovering sanitizer can preserve both expected stdout and exit status.
    if grep -Eqi 'runtime error:|UndefinedBehaviorSanitizer|AddressSanitizer|LeakSanitizer|MemorySanitizer|ThreadSanitizer' "$work/case.err"; then
        printf 'sanitizer report: %s' "$(head -n 1 "$work/case.err")"
        return 0
    fi
    if [[ "$status" != "$want_status" ]]; then
        printf 'exited %s, expected exactly %s: %s' \
            "$status" "$want_status" "$(head -n 1 "$work/case.err")"
        return 0
    fi
    if [[ "$expected" == "-" ]]; then
        if [[ -s "$work/case.out" ]]; then
            printf 'expected no stdout, got <%s>' "$(cat "$work/case.out")"
            return 0
        fi
    elif ! cmp -s "$work/case.out" "$expected"; then
        printf 'stdout differs from %s: got <%s>' \
            "$(basename "$expected")" "$(cat "$work/case.out")"
        return 0
    fi
    if [[ "$expected_err" == "-" ]]; then
        if [[ -s "$work/case.err" ]]; then
            printf 'expected no stderr, got <%s>' "$(cat "$work/case.err")"
        fi
    elif ! cmp -s "$work/case.err" "$expected_err"; then
        printf 'stderr differs from %s: got <%s>' \
            "$(basename "$expected_err")" "$(cat "$work/case.err")"
    fi
}

run_case() {  # <probe> <label> <status> <stdout-file|-> <stderr-file|-> <args...>
    local probe="$1" label="$2" want_status="$3" expected="$4" expected_err="$5"; shift 5
    local problem
    problem="$(check_case "$probe" "$want_status" "$expected" "$expected_err" "$@")"
    [[ -z "$problem" ]] || fail "$label: $problem"
    printf '  %-38s ok\n' "$label"
}

# Every case runs on BOTH builds.  The sanitized one makes undefined
# behaviour fatal, so a case that passes there passed without one - which is
# only worth anything if the cases reach the code that had it.
for variant in probe probe-ubsan; do
    tag="ordinary"
    [[ "$variant" == probe-ubsan ]] && tag="sanitized"

    # THE OPTION MUST DO SOMETHING.  With markers ON the same source emits the
    # marker AND the value; without this, "no markers" would be satisfied by a
    # preprocessor that emits none under any setting, which is the broken
    # state this guard exists to tell apart from the fixed one.
    run_case "$work/$variant" "$tag: markers on, marker and value" \
        "$EXIT_OK" "$work/markers_on.expected" - "$work/markers.cg"
    run_case "$work/$variant" "$tag: markers off, the value alone" \
        "$EXIT_OK" "$work/markers_off.expected" - --no-markers "$work/markers.cg"
    run_case "$work/$variant" "$tag: __FILE__ restored after include" \
        "$EXIT_OK" "$work/file_restore.expected" - \
        --no-markers -I "$work" "$work/file_restore.cg"
    # A saved filename must restore at BOTH include boundaries, while macros
    # defined in the leaf remain visible in the parent (no copied state).
    run_case "$work/$variant" "$tag: nested restore and retained macro" \
        "$EXIT_OK" "$work/nested.expected" - --no-markers -I "$work" "$work/nested.cg"

    # FUNCTION-LIKE EXPANSION, the case the uninitialised isVariadic was read
    # on.  A valid call must expand, and a call with the wrong argument count
    # must be refused BY NAME rather than by any failure.
    run_case "$work/$variant" "$tag: function-like macro expands" \
        "$EXIT_OK" "$work/funclike.expected" - --no-markers "$work/funclike.cg"
    run_case "$work/$variant" "$tag: wrong argument count is refused" \
        "$EXIT_THREW" - "$work/arity.err.expected" --no-markers "$work/arity.cg"
    # Skipping expansion of DROP's unused argument would silently accept ONE
    # with two arguments. It must still refuse for ONE's arity, not DROP's.
    run_case "$work/$variant" "$tag: unused argument still checks arity" \
        "$EXIT_THREW" - "$work/arity.err.expected" --no-markers "$work/unused_arity.cg"

    # An unreadable input must fail with its own status and no stdout.
    # Not because it could be mistaken for the markers-off answer - that
    # is "1" and a newline, and a byte comparison rejects an empty run
    # either way - but because a wrong path would otherwise be reported
    # as a successful run producing nothing, and every negative below
    # relies on empty output being an attributable failure.
    run_case "$work/$variant" "$tag: a missing source fails, with no stdout" \
        "$EXIT_INPUT" - "$work/missing.err.expected" --no-markers "$work/does-not-exist.cg"
    run_case "$work/$variant" "$tag: an empty source fails by name" \
        "$EXIT_INPUT" - "$work/empty.err.expected" --no-markers "$work/empty.cg"
done

# NEGATIVES.  Each stub would satisfy a looser assertion, and each must be
# rejected for its own reason - a guard nobody has watched reject something
# is not a guard.  Both refusal guards written for the texobj row passed a
# wrapper built to fool them before their self-checks were added.
printf '#!/usr/bin/env bash\nexit 0\n' > "$work/stub-empty"
chmod +x "$work/stub-empty"
problem="$(check_case "$work/stub-empty" "$EXIT_OK" "$work/markers_off.expected" - --no-markers "$work/markers.cg")"
[[ -n "$problem" ]] || fail \
    "a stub that prints NOTHING satisfied the markers-off case - the expectation is not distinguishing suppressed markers from no output at all"
case "$problem" in
    *"stdout differs"*) ;;
    *) fail "the empty stub was rejected for the wrong reason: $problem" ;;
esac

printf '"wrong.cg"\n"wrong.cg"\n"wrong.cg"\n' > "$work/wrong-names.txt"
{
    echo '#!/usr/bin/env bash'
    echo "cat '$work/wrong-names.txt'"
} > "$work/stub-wrong-names"
chmod +x "$work/stub-wrong-names"
problem="$(check_case "$work/stub-wrong-names" "$EXIT_OK" "$work/file_restore.expected" - --no-markers -I "$work" "$work/file_restore.cg")"
[[ -n "$problem" ]] || fail \
    "a stub that prints three names, all wrong, satisfied the __FILE__ case - the expectation is counting lines rather than reading them"
case "$problem" in
    *"stdout differs"*) ;;
    *) fail "the wrong-name stub was rejected for the wrong reason: $problem" ;;
esac

# A stub that expands the function-like macro but drops the trailing newline
# is the shape a command substitution CANNOT see: it strips trailing newlines
# from both sides, so the two compare equal.  The byte comparison above is
# what makes this rejectable, and this is the check that proves it.
printf '((1) + (2))' > "$work/no-newline.txt"
{
    echo '#!/usr/bin/env bash'
    echo "cat '$work/no-newline.txt'"
} > "$work/stub-no-newline"
chmod +x "$work/stub-no-newline"
problem="$(check_case "$work/stub-no-newline" "$EXIT_OK" "$work/funclike.expected" - --no-markers "$work/funclike.cg")"
[[ -n "$problem" ]] || fail \
    "a stub whose output is missing its trailing newline satisfied the function-like case - stdout is not being compared as bytes"
case "$problem" in
    *"stdout differs"*) ;;
    *) fail "the no-newline stub was rejected for the wrong reason: $problem" ;;
esac

# A successful exit and exact stdout must not conceal a sanitizer report.
{
    echo '#!/usr/bin/env bash'
    printf 'cat %q\n' "$work/funclike.expected"
    echo "echo 'probe.cpp:1: runtime error: load of value 254, which is not a valid value for type bool' >&2"
} > "$work/stub-sanitizer"
chmod +x "$work/stub-sanitizer"
problem="$(check_case "$work/stub-sanitizer" "$EXIT_OK" "$work/funclike.expected" -)"
case "$problem" in
    *"sanitizer report"*) ;;
    *) fail "sanitizer text with exact output/status was not rejected for its report: <$problem>" ;;
esac

# The same classifier must reject a correct refusal plus a sanitizer report,
# the correct status with a wrong diagnostic, timeout, and signal. Keep these
# controls IN the CI entry point, asserting their reasons as well as failure.
reject_case() {  # <label> <reason-prefix> <check_case args...>
    local label="$1" reason="$2" problem; shift 2
    problem="$(check_case "$@")"
    [[ "$problem" == "$reason"* ]] || fail "$label: expected '$reason', got <$problem>"
    printf '  control: %-36s rejected: %s\n' "$label" "$reason"
}

{
    echo '#!/usr/bin/env bash'
    printf 'cat %q >&2\n' "$work/arity.err.expected"
    echo "echo 'UndefinedBehaviorSanitizer: invalid bool' >&2"
    printf 'exit %s\n' "$EXIT_THREW"
} > "$work/stub-arity-sanitizer"
chmod +x "$work/stub-arity-sanitizer"
reject_case 'arity plus sanitizer' 'sanitizer report' \
    "$work/stub-arity-sanitizer" "$EXIT_THREW" - "$work/arity.err.expected"

{
    echo '#!/usr/bin/env bash'
    echo "echo 'preprocessor threw: Macro DROP called with incorrect number of arguments' >&2"
    printf 'exit %s\n' "$EXIT_THREW"
} > "$work/stub-wrong-reason"
chmod +x "$work/stub-wrong-reason"
reject_case 'wrong arity reason' 'stderr differs from arity.err.expected' \
    "$work/stub-wrong-reason" "$EXIT_THREW" - "$work/arity.err.expected"

# Preserve a real refusal's stdout and stderr, changing only its exit status.
for status in 124 134; do
    {
        echo '#!/usr/bin/env bash'
        printf 'cat %q >&2\n' "$work/arity.err.expected"
        printf 'exit %s\n' "$status"
    } > "$work/stub-status"
    chmod +x "$work/stub-status"
    reject_case "refusal status $status" "exited $status, expected exactly $EXIT_THREW" \
        "$work/stub-status" "$EXIT_THREW" - "$work/arity.err.expected"
done

# Also exercise the real timeout path; a fake exit 124 alone cannot prove
# that a hanging process is ever terminated. exec avoids orphaning a child.
printf '#!/usr/bin/env bash\nexec sleep 30\n' > "$work/stub-hang"
chmod +x "$work/stub-hang"
case_timeout=0.1s reject_case 'hanging probe' 'exited 124, expected exactly 0' \
    "$work/stub-hang" "$EXIT_OK" "$work/funclike.expected" -

printf 'preprocessor-harness-test: ok (18 ordinary/sanitized cases; 9 controls rejected for their expected reasons)\n'
printf 'PASS: preprocessor-harness-test\n'

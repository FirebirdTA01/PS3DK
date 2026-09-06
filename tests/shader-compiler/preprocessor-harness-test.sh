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
# make "exact" a claim this test did not check.  These fixtures are two and
# three lines long: anything that changes their output is worth a person
# reading it, including a change that looks harmless.
#
# THE SANITIZED PASS MUST EXPAND A FUNCTION-LIKE MACRO.  Object-like X and
# __FILE__ never reach the code that read the uninitialised bool, so a
# sanitized run over those alone would pass while the defect this row exists
# for went unexercised.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

src_dir="$repo_root/tools/rsx-cg-compiler/src/donor/frontend"
driver="$repo_root/tests/shader-compiler/harness/preprocessor_probe.cpp"
[[ -f "$driver" ]] || fail "harness driver missing: $driver"

cxx="${CXX:-g++}"
command -v "$cxx" >/dev/null 2>&1 || fail \
    "no C++ compiler ($cxx): this guard reaches the preprocessor API directly and cannot fall back to the command line, so it fails rather than reporting a green it did not earn"

work="${TMPDIR:-/tmp}/ps3dk-preprocessor-harness-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

build() {  # <output> <extra flags...>
    local out="$1"; shift
    "$cxx" -std=c++17 -O0 -g -I "$src_dir" "$@" \
        -o "$out" "$driver" "$src_dir/preprocessor.cpp" "$src_dir/lexer.cpp" \
        >"$out.build.log" 2>&1 || {
        tail -n 20 "$out.build.log" >&2
        fail "could not build the harness ($*)"
    }
}

build "$work/probe"
build "$work/probe-ubsan" -ftrivial-auto-var-init=pattern \
    -fsanitize=undefined -fno-sanitize-recover=all

# Fixtures.  Generated, because a .cg under tests/shaders is compiled by the
# tree sweeps and these are not shaders.
printf '#define X 1\nX\n' > "$work/markers.cg"
printf '__FILE__\n#include "child.h"\n__FILE__\n' > "$work/file_restore.cg"
printf '__FILE__\n' > "$work/child.h"
printf '#define ADD(a, b) ((a) + (b))\nADD(1, 2)\n' > "$work/funclike.cg"
printf '#define ONE(x) (x)\nONE(1, 2)\n' > "$work/arity.cg"

# Expected stdout, byte for byte.
printf '#line 2 "%s"\n1\n' "$work/markers.cg" > "$work/markers_on.expected"
printf '1\n' > "$work/markers_off.expected"
printf '"%s"\n"%s"\n"%s"\n' \
    "$work/file_restore.cg" "$work/child.h" "$work/file_restore.cg" \
    > "$work/file_restore.expected"
printf '((1) + (2))\n' > "$work/funclike.expected"

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
check_case() {  # <probe> <expected-status> <expected-stdout-file|-> <args...>
    local probe="$1" want_status="$2" expected="$3"; shift 3
    local status
    set +e
    "$probe" "$@" >"$work/case.out" 2>"$work/case.err"
    status=$?
    set -e
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
        return 0
    fi
    if ! cmp -s "$work/case.out" "$expected"; then
        printf 'stdout differs from %s: got <%s>' \
            "$(basename "$expected")" "$(cat "$work/case.out")"
        return 0
    fi
}

run_case() {  # <probe> <label> <expected-status> <expected-file|-> <args...>
    local probe="$1" label="$2" want_status="$3" expected="$4"; shift 4
    local problem
    problem="$(check_case "$probe" "$want_status" "$expected" "$@")"
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
        "$EXIT_OK" "$work/markers_on.expected" "$work/markers.cg"
    run_case "$work/$variant" "$tag: markers off, the value alone" \
        "$EXIT_OK" "$work/markers_off.expected" --no-markers "$work/markers.cg"
    run_case "$work/$variant" "$tag: __FILE__ restored after include" \
        "$EXIT_OK" "$work/file_restore.expected" \
        --no-markers -I "$work" "$work/file_restore.cg"

    # FUNCTION-LIKE EXPANSION, the case the uninitialised isVariadic was read
    # on.  A valid call must expand, and a call with the wrong argument count
    # must be refused BY NAME rather than by any failure.
    run_case "$work/$variant" "$tag: function-like macro expands" \
        "$EXIT_OK" "$work/funclike.expected" --no-markers "$work/funclike.cg"
    run_case "$work/$variant" "$tag: wrong argument count is refused" \
        "$EXIT_THREW" "-" --no-markers "$work/arity.cg"
    grep -q "Macro ONE called with incorrect number of arguments" "$work/case.err" || {
        head -n 2 "$work/case.err" >&2
        fail "$tag: the arity refusal did not name the macro and the rule"
    }

    # An unreadable input must fail with its own status and no stdout.
    # Not because it could be mistaken for the markers-off answer - that
    # is "1" and a newline, and a byte comparison rejects an empty run
    # either way - but because a wrong path would otherwise be reported
    # as a successful run producing nothing, and every negative below
    # relies on empty output being an attributable failure.
    run_case "$work/$variant" "$tag: a missing source fails, with no stdout" \
        "$EXIT_INPUT" "-" --no-markers "$work/does-not-exist.cg"
    grep -q "cannot open" "$work/case.err" || \
        fail "$tag: the driver failed on a missing source without saying so"

    if grep -qi "runtime error" "$work/case.err"; then
        fail "$tag: the run reported undefined behaviour: $(head -n 1 "$work/case.err")"
    fi
done

# NEGATIVES.  Each stub would satisfy a looser assertion, and each must be
# rejected for its own reason - a guard nobody has watched reject something
# is not a guard.  Both refusal guards written for the texobj row passed a
# wrapper built to fool them before their self-checks were added.
printf '#!/usr/bin/env bash\nexit 0\n' > "$work/stub-empty"
chmod +x "$work/stub-empty"
problem="$(check_case "$work/stub-empty" "$EXIT_OK" "$work/markers_off.expected" --no-markers "$work/markers.cg")"
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
problem="$(check_case "$work/stub-wrong-names" "$EXIT_OK" "$work/file_restore.expected" --no-markers -I "$work" "$work/file_restore.cg")"
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
problem="$(check_case "$work/stub-no-newline" "$EXIT_OK" "$work/funclike.expected" --no-markers "$work/funclike.cg")"
[[ -n "$problem" ]] || fail \
    "a stub whose output is missing its trailing newline satisfied the function-like case - stdout is not being compared as bytes"

printf 'preprocessor-harness-test: ok (exact bytes for both marker settings, the __FILE__ sequence and function-like expansion, on ordinary and sanitized builds; three negatives rejected)\n'
printf 'PASS: preprocessor-harness-test\n'

#!/usr/bin/env bash
# refusal-status-adversary-test.sh - a guard that cannot tell a refusal from a
# crash is not a guard (t_fd95d1b9).
#
# Every script in this directory that asserts "the compiler must REFUSE this"
# is asserting something about an exit status, and `did not exit 0` is not that
# assertion: 124 is a timeout and >= 128 is a signal, and both satisfy it while
# meaning the compiler never reached the decision the guard is about.  Measured
# on the tree before this landed: of the 32 scripts here that assert a refusal,
# SIXTEEN passed unchanged when every refusal was turned into a SIGABRT.  A
# compiler that crashed on a shader it should have refused BY NAME would have
# been reported as correct by half the guards written to catch exactly that.
#
# The property cannot be checked by reading the scripts.  Three different
# spellings were in use - `[[ $rc -ne 0 ]] || fail`, `if [[ $rc -eq 0 ]]; then
# fail`, and body-only assertions that check the diagnostic and never look at
# the status at all - so a grep for one of them finds a third of the problem
# and reports the rest as clean.  This runs the suite against a compiler that
# crashes instead of refusing and asks which scripts still say ok.
#
# TRANSPARENCY IS THE WHOLE THING.  The adversary must differ from the real
# compiler in the exit status of a REFUSAL and in nothing else - same stdout,
# same stderr, same container, same status on every compile that succeeds and
# on every invocation that is not a compile at all.  A wrapper that merged the
# two streams, or that swallowed output, would make scripts fail for its own
# reasons and every one of those would look like a finding.  That is checked
# below, against the real compiler, before the adversary is used to judge
# anything.
#
# UNDER-REPORTING IS THE SAFE DIRECTION and it is deliberate: a script that
# fails under the adversary for an unrelated reason (no host g++, say) is
# recorded as having caught the crash rather than as a false green.  This test
# can miss a weak guard; it cannot invent one.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"
compiler="$(cd "$(dirname "$compiler")" && pwd -P)/$(basename "$compiler")"

work="${TMPDIR:-/tmp}/ps3dk-refusal-adversary-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# ---------------------------------------------------------------- the adversary
# Identical to the compiler except that a refusal (exit 1) becomes a SIGABRT.
# stdout and stderr are INHERITED, not captured and replayed, so their contents,
# their interleaving and their separation are the real compiler's.
wrap="$work/crash-on-refusal-cc"
fired="$work/fired"
cat >"$wrap" <<WRAP
#!/usr/bin/env bash
"$compiler" "\$@"
rc=\$?
if [ "\$rc" -eq 1 ]; then
    echo x >> "$fired"
    kill -ABRT \$\$
fi
exit \$rc
WRAP
chmod +x "$wrap"

# ------------------------------------------------------- transparency self-check
# Run the same invocation both ways and require the two to be indistinguishable
# wherever the adversary is supposed to be indistinguishable.
probe() {   # $1 tag, then the arguments; leaves $work/$1.{real,wrap}.{out,err,rc}
    local tag="$1"; shift
    "$compiler" "$@" >"$work/$tag.real.out" 2>"$work/$tag.real.err" \
        && printf '0' >"$work/$tag.real.rc" || printf '%s' "$?" >"$work/$tag.real.rc"
    : >"$fired"
    # The braces catch the SHELL's own "Aborted (core dumped)" notice, which it
    # writes to ITS stderr when the adversary aborts; the command's own streams
    # are already redirected to the files being compared.
    { "$wrap" "$@" >"$work/$tag.wrap.out" 2>"$work/$tag.wrap.err" \
        && printf '0' >"$work/$tag.wrap.rc" || printf '%s' "$?" >"$work/$tag.wrap.rc"
    } 2>/dev/null
}

same_streams() {   # $1 tag, $2 what
    cmp -s "$work/$1.real.out" "$work/$1.wrap.out" \
        || fail "the adversary changed STDOUT on $2; it must differ from the compiler in the exit status of a refusal and in nothing else"
    cmp -s "$work/$1.real.err" "$work/$1.wrap.err" \
        || fail "the adversary changed STDERR on $2; a wrapper that rewrites diagnostics makes every guard below fail for its own reasons"
}

# 1. an invocation that is not a compile at all
probe version --version
same_streams version "--version"
[[ "$(cat "$work/version.real.rc")" == "$(cat "$work/version.wrap.rc")" ]] \
    || fail "the adversary changed the exit status of --version"

probe listext --list-extensions
same_streams listext "--list-extensions"
[[ "$(cat "$work/listext.real.rc")" == "$(cat "$work/listext.wrap.rc")" ]] \
    || fail "the adversary changed the exit status of --list-extensions"

# 2. a compile that SUCCEEDS - streams, status and the emitted container
ok_src="$shaders/fp_discard_lt_f.cg"
[[ -f "$ok_src" ]] || fail "fixture missing: $ok_src"
"$compiler" -p sce_fp_rsx --emit-container "$work/ok.real.fpo" "$ok_src" \
    >"$work/okc.real.out" 2>"$work/okc.real.err" || fail "the transparency fixture does not compile: $ok_src"
"$wrap" -p sce_fp_rsx --emit-container "$work/ok.wrap.fpo" "$ok_src" \
    >"$work/okc.wrap.out" 2>"$work/okc.wrap.err" || fail "the adversary refused a shader the compiler accepts"
cmp -s "$work/okc.real.out" "$work/okc.wrap.out" || fail "the adversary changed STDOUT on a successful compile"
cmp -s "$work/okc.real.err" "$work/okc.wrap.err" || fail "the adversary changed STDERR on a successful compile"
cmp -s "$work/ok.real.fpo" "$work/ok.wrap.fpo"   || fail "the adversary changed the EMITTED CONTAINER; it must only move the status of a refusal"

# 3. a compile that REFUSES - the diagnostic must survive untouched and only
#    the status may move.  If this does not fire, the adversary is inert and
#    every "caught" below would be meaningless.
bad_src="$shaders/fp_local_array_dynamic_index_f.cg"
[[ -f "$bad_src" ]] || fail "fixture missing: $bad_src"
probe refuse -p sce_fp_rsx "$bad_src"
same_streams refuse "a refused compile"
[[ "$(cat "$work/refuse.real.rc")" == "1" ]] \
    || fail "the refusal fixture exited $(cat "$work/refuse.real.rc"), not 1; pick a fixture that refuses"
[[ "$(cat "$work/refuse.wrap.rc")" == "134" ]] \
    || fail "the adversary did not turn a refusal into a SIGABRT (got $(cat "$work/refuse.wrap.rc")); it is inert and proves nothing"
[[ -s "$fired" ]] || fail "the adversary's counter did not record the refusal it converted"

printf '  transparency: identical stdout, stderr, container and status except a refusal 1 -> 134\n'

# --------------------------------------------------- the detector, and its own test
# judge() runs one script under the adversary and echoes its verdict.  A script
# is only judged when the adversary actually FIRED for it: "did not exercise a
# refusal" is not a finding, and counting it as one is how this measurement was
# first over-stated by a script that sweeps the corpus and ignores refusals.
judge() {   # $1 script, $2 timeout -> weak | ok | hung | broke:N | untested
    local script="$1" rc=0
    local tmo="${2:-${PS3TC_ADVERSARY_TIMEOUT:-400s}}"
    : >"$fired"
    timeout "$tmo" bash "$script" "$wrap" >"$work/judge.log" 2>&1 || rc=$?
    # A script that never compiled anything that refuses is not judged at all,
    # whatever its status: it may have failed for a reason of its own (this
    # host has no g++ for two of them, and they exit 127) and calling that a
    # finding is how the measurement behind this test was first overstated.
    #
    # With ONE exception, because otherwise an infrastructure failure vanishes
    # into the safest-looking bucket: a script that HUNG or died on a signal
    # BEFORE it ever reached a refusing compile has not been let off, it has
    # not been examined, and "untested" would read as "nothing to see".
    if [[ ! -s "$fired" ]]; then
        case "$rc" in
            124) echo hung ;;
            *)   if (( rc >= 128 )); then echo "broke:$rc"; else echo untested; fi ;;
        esac
        return 0
    fi
    # `rc is not zero` is the very thing this file exists to reject, and it
    # would be wrong HERE for the same reason it is wrong in the guards: a
    # script that HUNG or died under the adversary did not decide anything, and
    # reading either as "caught it" would be this defect one level up (codex,
    # on the first draft, which did exactly that).  Only the scripts' own
    # fail() - exit 1 - counts as having caught the crash.
    case "$rc" in
        0)   echo weak ;;
        1)   echo ok ;;
        124) echo hung ;;
        *)   echo "broke:$rc" ;;
    esac
}

# A detector nobody has seen accuse anything is not a detector.  Two synthetic
# scripts, one weak and one correct, both asserting the same refusal.
cat >"$work/weak-test.sh" <<WEAK
#!/usr/bin/env bash
set -euo pipefail
cc="\$1"
rc=0
"\$cc" -p sce_fp_rsx "$bad_src" >/dev/null 2>&1 || rc=\$?
[[ "\$rc" -ne 0 ]] || { echo "FAIL: compiled" >&2; exit 1; }
echo ok
WEAK
cat >"$work/strong-test.sh" <<STRONG
#!/usr/bin/env bash
set -euo pipefail
cc="\$1"
rc=0
"\$cc" -p sce_fp_rsx "$bad_src" >/dev/null 2>&1 || rc=\$?
[[ "\$rc" -eq 1 ]] || { echo "FAIL: a refusal is exit 1, got \$rc" >&2; exit 1; }
echo ok
STRONG
# ... and two more that fire and then never decide, because "did not exit 0"
# would read both of them as a pass.
cat >"$work/hang-test.sh" <<HANG
#!/usr/bin/env bash
set -euo pipefail
cc="\$1"
"\$cc" -p sce_fp_rsx "$bad_src" >/dev/null 2>&1 || true
sleep 600
HANG
cat >"$work/broke-test.sh" <<BROKE
#!/usr/bin/env bash
set -euo pipefail
cc="\$1"
"\$cc" -p sce_fp_rsx "$bad_src" >/dev/null 2>&1 || true
exit 3
BROKE
cat >"$work/prefire-hang-test.sh" <<PREFIRE
#!/usr/bin/env bash
set -euo pipefail
sleep 600
PREFIRE
chmod +x "$work/weak-test.sh" "$work/strong-test.sh" "$work/hang-test.sh" \
         "$work/broke-test.sh" "$work/prefire-hang-test.sh"

[[ "$(judge "$work/weak-test.sh")" == weak ]] \
    || fail "the detector did not accuse a script that asserts a refusal as 'not zero' - it would report the whole suite clean"
[[ "$(judge "$work/strong-test.sh")" == ok ]] \
    || fail "the detector accused a script that asserts exit 1 exactly - it would accuse every correct guard"
[[ "$(judge "$work/hang-test.sh" 2s)" == hung ]] \
    || fail "the detector read a script that FIRED AND THEN HUNG as a result; a timeout is not a verdict"
[[ "$(judge "$work/broke-test.sh")" == broke:3 ]] \
    || fail "the detector read a script that FIRED AND THEN DIED (exit 3) as a result; only a script's own fail() counts as catching the crash"
[[ "$(judge "$work/prefire-hang-test.sh" 2s)" == hung ]] \
    || fail "the detector filed a script that HUNG BEFORE reaching any refusal under 'never compiled one'; an infrastructure failure must not vanish into the quietest bucket"
printf '  self-check: accuses a `-ne 0` guard, clears an `-eq 1` guard, and scores neither a hang nor a crash of its own - before or after the counter fires\n'

# ------------------------------------------------------------------ the real run
mapfile -t scripts < <(cd "$here" && ls *-test.sh | grep -v '^refusal-status-adversary-test\.sh$' | sort)
(( ${#scripts[@]} > 20 )) || fail "only ${#scripts[@]} sibling test scripts found - the enumeration broke, so nothing was judged"

weak=(); inconclusive=(); ok=0; untested=0
for s in "${scripts[@]}"; do
    verdict="$(judge "$here/$s")"
    [[ -z "${PS3TC_ADVERSARY_VERBOSE:-}" ]] || printf '    %-44s %s
' "$s" "$verdict"
    case "$verdict" in
        weak)     weak+=("$s") ;;
        ok)       ok=$((ok + 1)) ;;
        untested) untested=$((untested + 1)) ;;
        *)        inconclusive+=("$s ($verdict)") ;;
    esac
done

printf '  judged %d scripts: %d assert a refusal and catch a crash, %d never compile one, %d weak, %d inconclusive\n' \
    "${#scripts[@]}" "$ok" "$untested" "${#weak[@]}" "${#inconclusive[@]}"

# "untested" is the safe verdict and it is also the one that can hide a
# systemic failure: if the adversary stopped firing - a broken wrapper, a
# compiler that refuses nothing, a host slow enough that the scripts' own
# timeouts kill a compile before it can refuse - every script would come back
# untested and this test would report a clean suite having judged nothing.  The
# count is not asserted exactly, because a legitimate new script moves it; the
# floor is.  (One run in four here judged 32 rather than 33, which is that
# effect in miniature: a script whose own timeout fired first is recorded as
# untested, which is the safe direction and still worth being able to see.)
judged=$(( ok + ${#weak[@]} + ${#inconclusive[@]} ))
(( judged >= 20 )) || fail "only $judged of ${#scripts[@]} scripts compiled anything that refuses.
The adversary is not reaching the suite, so a clean result here would mean
nothing.  Check that the wrapper still fires and that the scripts' own
timeouts are not killing compiles before they can refuse."

# An inconclusive script is not a pass.  It compiled something that refuses and
# then hung or died, so this test learned nothing about it - and a measurement
# that cannot see a guard must say so rather than count it green.
if (( ${#inconclusive[@]} > 0 )); then
    printf 'FAIL: these scripts compiled a refusal and then hung or died under the\nadversary, so nothing was established about their guards:\n' >&2
    printf '  %s\n' "${inconclusive[@]}" >&2
    exit 1
fi

if (( ${#weak[@]} > 0 )); then
    printf 'FAIL: these guards report a refusal when the compiler CRASHED instead:\n' >&2
    printf '  %s\n' "${weak[@]}" >&2
    printf '\nAssert the status a refusal actually has - exit 1 - and name a timeout\n(124) and a signal (>= 128) separately, so a crash cannot be read as the\nrefusal the guard is about.  See refusal_status() in the sibling scripts.\n' >&2
    exit 1
fi

printf 'PASS: refusal-status-adversary-test\n'

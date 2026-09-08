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
# A failed script is never called untested: entered and fired counters separate
# failure before wrapper entry from failure without a refusal. Only a successful
# script that exercised no refusal is untested. A failure AFTER firing can still
# be unrelated to the crash; "caught" does not establish its diagnostic's cause.
# Windows needs Windows PowerShell/.NET Framework to build the native launcher,
# and an explicit Git Bash (PS3TC_ADVERSARY_BASH, default this running Bash).
# A batch file is insufficient: cmd.exe parses the command line before its body.
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
body="$wrap"
fired="$work/fired"
entered="$work/entered"
{
printf '%s\n' '#!/usr/bin/env bash'
printf 'compiler=%q\nentered=%q\nfired=%q\n' "$compiler" "$entered" "$fired"
cat <<'WRAP'
echo x >> "$entered"
"$compiler" "$@"
rc=$?
if [ "$rc" -eq 1 ]; then
    echo x >> "$fired"
    kill -ABRT $$
fi
exit $rc
WRAP
} >"$body"
chmod +x "$wrap"

case "$OSTYPE" in
    msys*|cygwin*)
        native_bash="${PS3TC_ADVERSARY_BASH:-$(cygpath -am "$BASH")}"
        [[ "$native_bash" == [A-Za-z]:/* || "$native_bash" == [A-Za-z]:\\* ]] \
            && [[ -x "$native_bash" ]] \
            || fail "inconclusive: launcher prerequisite: explicit Git Bash not executable: $native_bash"
        windows_root="${SYSTEMROOT:-${SystemRoot:-}}"
        [[ -n "$windows_root" ]] || fail 'inconclusive: launcher prerequisite: Windows system root missing'
        powershell="$(cygpath -am "$windows_root")/System32/WindowsPowerShell/v1.0/powershell.exe"
        [[ -x "$powershell" ]] \
            || fail "inconclusive: launcher prerequisite: Windows PowerShell/.NET Framework missing: $powershell"
        wrap="$body.exe"
        "$powershell" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
            -File "$(cygpath -am "$here/build-adversary-launcher.ps1")" \
            -Source "$(cygpath -am "$here/adversary-launcher.cs")" \
            -Output "$(cygpath -am "$wrap")" >"$work/launcher-build.log" 2>&1 \
            || { cat "$work/launcher-build.log" >&2; fail 'inconclusive: launcher prerequisite: Windows PowerShell/.NET Framework compilation failed'; }
        printf '%s\n%s\n' "$native_bash" "$(cygpath -am "$body")" >"$wrap.paths"
        ;;
    *) native_bash="$BASH" ;;
esac

# This checks native Python AND Bash entry, argv (including MSYS path forms),
# binary streams, status and artifact bytes. Its corrupted controls assert the
# reason, so a transport failure cannot masquerade as a guard verdict.
python3 "$here/adversary_transport_check.py" "$wrap" "$native_bash" "$work" "$compiler" "$shaders" \
    || fail 'inconclusive: launcher transport self-check failed'

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
judge() {   # $1 script, $2 timeout -> verdict (failures without firing are named)
    local script="$1" rc=0
    local tmo="${2:-${PS3TC_ADVERSARY_TIMEOUT:-400s}}"
    : >"$fired"
    : >"$entered"
    timeout "$tmo" bash "$script" "$wrap" >"$work/judge.log" 2>&1 || rc=$?
    # No entry is evidence of where execution stopped, not why. In particular
    # a host-toolchain failure and a wrapper launch failure must both remain
    # inconclusive, with the script's diagnostic printed by the caller.
    if [[ ! -s "$fired" ]]; then
        case "$rc" in
            124) echo hung ;;
            0) echo untested ;;
            *) if (( rc >= 128 )); then echo "broke:$rc"
               elif [[ ! -s "$entered" ]]; then echo "failed-before-wrapper:$rc"
               else echo "failed-without-refusal:$rc"; fi ;;
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

# ONLY names guards that are EXPECTED to exercise a refusal. Unlike the full
# inventory (which includes success-only and host API tests), every selected
# guard must reach a refusal, or the targeted run has not proved its property.
require_targeted_refusals() {
    if (( $# > 0 )); then
        printf 'FAIL: targeted guards exercised no refusal:\n' >&2
        printf '  %s\n' "$@" >&2
        return 1
    fi
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

# Python launches exactly the same executable, not `bash wrapper`. Pin the crash
# explicitly as well as accusing !=0: a broken launcher exiting 125 could
# otherwise satisfy both the weak/strong tests without simulating the crash.
for kind in weak strong status; do
    cat >"$work/python-$kind-test.sh" <<PYGUARD
#!/usr/bin/env bash
python3 - "\$1" "$bad_src" "$kind" <<'PY'
import os, subprocess, sys
rc = subprocess.run([sys.argv[1], '-p', 'sce_fp_rsx', sys.argv[2]],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
crash = 134 if os.name == 'nt' else -6  # Python reports POSIX signals negatively.
ok = rc != 0 if sys.argv[3] == 'weak' else rc == (crash if sys.argv[3] == 'status' else 1)
sys.exit(0 if ok else 1)
PY
PYGUARD
done
[[ "$(judge "$work/python-weak-test.sh")" == weak ]] \
    || fail 'Python control: a !=0 guard did not accept the crash'
[[ "$(judge "$work/python-strong-test.sh")" == ok ]] \
    || fail 'Python control: an exact-1 guard did not catch the crash'
[[ "$(judge "$work/python-status-test.sh")" == weak ]] \
    || fail 'Python control: SIGABRT did not arrive as 134 on Windows / -6 on POSIX'

cat >"$work/launch-failed-test.sh" <<'LAUNCH'
#!/usr/bin/env bash
python3 - "$1.missing" <<'PY'
import subprocess, sys
subprocess.run([sys.argv[1]])
PY
LAUNCH
cat >"$work/no-refusal-test.sh" <<'NOREFUSAL'
#!/usr/bin/env bash
"$1" --version >/dev/null 2>&1 || exit 3
exit 0
NOREFUSAL
cat >"$work/failed-no-refusal-test.sh" <<'FAILED'
#!/usr/bin/env bash
"$1" --version >/dev/null 2>&1 || exit 3
exit 2
FAILED
[[ "$(judge "$work/launch-failed-test.sh")" == failed-before-wrapper:1 ]] \
    || fail 'launch-failure control: failure before entry was not named inconclusive'
grep -q 'FileNotFoundError' "$work/judge.log" \
    || fail 'launch-failure control: the expected launch diagnostic was absent'
[[ "$(judge "$work/no-refusal-test.sh")" == untested ]] \
    || fail 'no-refusal control: successful non-refusal invocation was accused'
[[ "$(judge "$work/failed-no-refusal-test.sh")" == failed-without-refusal:2 ]] \
    || fail 'failed-no-refusal control: failure after entry vanished into untested'

# A swallowed launch failure exits successfully with no entry/refusal. The
# ordinary classifier correctly has no refusal to judge; the TARGETED gate
# must reject it because the caller promised this named guard exercises one.
cat >"$work/unreachable-target-test.sh" <<'UNREACHABLE'
#!/usr/bin/env bash
python3 - "$1.missing" <<'PY'
import subprocess, sys
try:
    subprocess.run([sys.argv[1]])
except FileNotFoundError:
    print('launcher unreachable: FileNotFoundError', file=sys.stderr)
else:
    sys.exit(3)
PY
UNREACHABLE
[[ "$(judge "$work/unreachable-target-test.sh")" == untested ]] \
    || fail 'targeted control: swallowed launch failure did not reach the no-refusal class'
grep -Fxq 'launcher unreachable: FileNotFoundError' "$work/judge.log" \
    || fail 'targeted control: the launcher was not unreachable for the expected reason'
if require_targeted_refusals unreachable-target-test.sh >"$work/targeted-control.log" 2>&1; then
    fail 'targeted control: a successful target with an unreachable launcher was accepted without exercising a refusal'
fi
grep -Fxq 'FAIL: targeted guards exercised no refusal:' "$work/targeted-control.log" \
    && grep -Fxq '  unreachable-target-test.sh' "$work/targeted-control.log" \
    || fail 'targeted control: rejection did not name the missing refusal and its target'
printf '  targeted control rejected: unreachable-target-test.sh exercised no refusal\n'
printf '  detector controls: Bash/Python weak accused, exact-1 caught; Python SIGABRT=134 (Windows)/-6 (POSIX); launch failure=failed-before-wrapper:1; no refusal=untested; entered failure=failed-without-refusal:2; hangs=hung; crash=broke:3\n'

# ------------------------------------------------------------------ the real run
mapfile -t scripts < <(cd "$here" && ls *-test.sh | grep -v '^refusal-status-adversary-test\.sh$' | sort)
(( ${#scripts[@]} > 20 )) || fail "only ${#scripts[@]} sibling test scripts found - the enumeration broke, so nothing was judged"
if [[ -n "${PS3TC_ADVERSARY_ONLY:-}" ]]; then
    read -r -a selected <<<"$PS3TC_ADVERSARY_ONLY"
    (( ${#selected[@]} > 0 )) || fail 'empty selected guard list'
    declare -A selected_seen=()
    for s in "${selected[@]}"; do
        printf '%s\n' "${scripts[@]}" | grep -Fxq "$s" || fail "unknown selected guard: $s"
        [[ -z "${selected_seen[$s]:-}" ]] || fail "duplicate selected guard: $s"
        selected_seen[$s]=1
    done
    scripts=("${selected[@]}")
    printf '  targeted run: %d named guards (not the full inventory)\n' "${#scripts[@]}"
fi

weak=(); inconclusive=(); untested_names=(); ok=0; untested=0
for s in "${scripts[@]}"; do
    started=$SECONDS
    verdict="$(judge "$here/$s")"
    [[ -z "${PS3TC_ADVERSARY_VERBOSE:-}" ]] || printf '    %-44s %s elapsed=%ss\n' "$s" "$verdict" "$((SECONDS - started))"
    case "$verdict" in
        weak)     weak+=("$s") ;;
        ok)       ok=$((ok + 1)) ;;
        untested) untested=$((untested + 1)); untested_names+=("$s") ;;
        *)        inconclusive+=("$s ($verdict)")
                  printf '  inconclusive: %s (%s)\n' "$s" "$verdict" >&2
                  tail -n 8 "$work/judge.log" >&2 ;;
    esac
done

printf '  judged %d scripts: %d assert a refusal and catch a crash, %d never compile one, %d weak, %d inconclusive\n' \
    "${#scripts[@]}" "$ok" "$untested" "${#weak[@]}" "${#inconclusive[@]}"

if [[ -n "${PS3TC_ADVERSARY_ONLY:-}" ]]; then
    require_targeted_refusals "${untested_names[@]}" || exit 1
fi

# "untested" is the safe verdict and it is also the one that can hide a
# systemic failure: if the adversary stopped firing - a broken wrapper, a
# compiler that refuses nothing, a host slow enough that the scripts' own
# timeouts kill a compile before it can refuse - every script would come back
# untested and this test would report a clean suite having judged nothing.  The
# count is not asserted exactly, because a legitimate new script moves it; the
# floor is. Explicitly selected runs identify their smaller population and
# still fail on any weak or inconclusive row.
judged=$(( ok + ${#weak[@]} + ${#inconclusive[@]} ))
[[ -n "${PS3TC_ADVERSARY_ONLY:-}" ]] || (( judged >= 20 )) || fail "only $judged of ${#scripts[@]} scripts compiled anything that refuses.
The adversary is not reaching the suite, so a clean result here would mean
nothing.  Check that the wrapper still fires and that the scripts' own
timeouts are not killing compiles before they can refuse."

# An inconclusive script is not a pass, including a failed script that never
# reached the wrapper. Its diagnostic is printed above beside its name.
if (( ${#inconclusive[@]} > 0 )); then
    printf 'FAIL: these scripts could not be judged (failure before wrapper entry,\nfailure without refusal, timeout or abnormal exit):\n' >&2
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

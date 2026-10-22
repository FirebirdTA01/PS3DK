#!/usr/bin/env bash
# Host self-test of driver-tsan.sh (the TSan launch path of build-and-run.sh) with FAKE children only: no compiler, no
# TSan, no setarch personality change.  Each case runs in its own bash process with the driver's own shell options
# (set -euo pipefail), sources the real driver-tsan.sh, injects one fault and calls the driver entry points in the
# driver's own conditional contexts:
#   if ! admit_variant tsan; then ...        (build-and-run.sh: the build loop; tsan_wrapper_ok + sanitizer_links)
#   if ! admit_tsan_controls; then ...       (build-and-run.sh: the run loop; tsan_controls)
# plus direct checks of the exact status codes of xrun, tsan_wrapper_ok, sanitizer_links and tsan_controls.
#
#   bash driver-tsan-selftest.sh <ABSOLUTE work dir (must not exist)>
#
# The fake tools live in a per-case bin dir; PATH is that dir plus a tool dir holding wrappers for only the real tools
# the driver needs, so a real setarch elsewhere on the host can never be found.  Every fake child appends its name to
# launch.log when it runs, and the fake setarch logs each call to setarch.log, so a case can prove that a child did NOT
# run.  Exit 0 only when every case matches its expectation exactly.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- case mode: one case in this process -----------------------------------------------------------------------------
if [ "${1:-}" = --case ]; then
    name="$2" cdir="$3"
    out="$cdir/out"
    mkdir -p "$out"
    CC="$cdir/bin/cc"
    warn=(-Wall -Wextra -Werror)
    declare -A vflags=([plain]="" [asan]="-fsanitize=address,undefined" [tsan]="-fsanitize=thread")
    declare -A notbuilt=()
    REQUIRE_SANITIZERS=1
    pass=0 fail=0 skip=0
    ok() { pass=$((pass + 1)); echo "PASS $*"; }
    bad() { fail=$((fail + 1)); echo "FAIL $*"; }
    skp() { skip=$((skip + 1)); echo "SKIP $*"; }
    PATH="$cdir/bin:$cdir/tools"
    export PATH
    # shellcheck source=driver-tsan.sh
    . "$here/driver-tsan.sh"
    controls="$out/controls.tsv"
    printf 'control\tresult\treason\n' >"$controls"

    # fault injection: a record path made into a directory, or a fake tool told to fail
    fault_dir() { mkdir -p "$out/$1"; }
    admitted=NA controls_ok=NA st=NA
    case "$name" in
    probe-argv) fault_dir probe-tsan/run.argv ;;
    probe-env) fault_dir probe-tsan/run.env ;;
    probe-stdout) fault_dir probe-tsan/run.stdout ;;
    probe-stderr) fault_dir probe-tsan/run.stderr ;;
    probe-rc) fault_dir probe-tsan/run.rc ;;
    ctl-pos-argv) fault_dir controls-tsan/positive.argv ;;
    ctl-pos-env) fault_dir controls-tsan/positive.env ;;
    ctl-pos-rc) fault_dir controls-tsan/positive.rc ;;
    ctl-neg-argv) fault_dir controls-tsan/negative.argv ;;
    ctl-tsv) rm -f "$controls"; mkdir -p "$controls" ;;
    id-commandv) rm -f "$cdir/bin/setarch" ;;
    id-realpath) echo realpath >"$cdir/fault" ;;
    id-sha256) echo sha256 >"$cdir/fault" ;;
    id-version) echo version >"$cdir/fault" ;;
    id-personality) echo personality >"$cdir/fault" ;;
    id-write) fault_dir setarch/command-v.txt ;;
    esac

    case "$name" in
    xrun-*)
        setarch="$cdir/bin/setarch"
        mkdir -p "$out/x"
        case "$name" in
        xrun-argv) mkdir -p "$out/x/p.argv" ;;
        xrun-rc) mkdir -p "$out/x/rc" ;;
        esac
        st=0
        xrun "$out/x/p" "$out/x/stdout" "$out/x/stderr" "$out/x/rc" 5 "$cdir/bin/exit7" || st=$?
        echo "RESULT st=$st xrc=$xrc recfail=$recfail"
        exit 0
        ;;
    direct-wrapper-*)
        st=0
        tsan_wrapper_ok || st=$?
        echo "RESULT st=$st recfail=$recfail"
        exit 0
        ;;
    direct-probe-argv)
        setarch="$cdir/bin/setarch"
        mkdir -p "$out/probe-tsan/run.argv"
        st=0
        sanitizer_links tsan || st=$?
        echo "RESULT st=$st recfail=$recfail"
        exit 0
        ;;
    direct-controls-rc)
        setarch="$cdir/bin/setarch"
        mkdir -p "$out/controls-tsan/positive.rc"
        st=0
        tsan_controls || st=$?
        echo "RESULT st=$st recfail=$recfail"
        exit 0
        ;;
    esac

    # the driver's own calling contexts
    if ! admit_variant tsan; then
        admitted=0
    else
        admitted=1
    fi
    if [ "$admitted" = 1 ]; then
        if ! admit_tsan_controls; then
            controls_ok=0
        else
            controls_ok=1
        fi
    fi
    echo "RESULT admitted=$admitted controls=$controls_ok recfail=$recfail fail=$fail notbuilt=${notbuilt[tsan]:-}"
    exit 0
fi

# ---- driver mode -----------------------------------------------------------------------------------------------------
case "${1:-}" in
/*) ;;
*)
    echo "usage: bash driver-tsan-selftest.sh <ABSOLUTE work dir (must not exist)>; got '${1:-}'" >&2
    exit 2
    ;;
esac
W="$1"
if [ -e "$W" ]; then
    echo "refused: $W exists" >&2
    exit 3
fi
mkdir -p "$W"

# Real tools the driver path needs, resolved once; each case gets wrappers for exactly these.
tools=(bash sh env timeout readlink sha256sum cat mkdir rm printf grep awk head true chmod)
declare -A real=()
for t in "${tools[@]}"; do
    real[$t]="$(type -P "$t")" || { echo "selftest: no $t on PATH" >&2; exit 1; }
done

mkcase() {
    local c="$W/$1" t
    mkdir -p "$c/bin" "$c/tools"
    for t in "${tools[@]}"; do
        printf '#!%s\nexec %s "$@"\n' "${real[sh]}" "${real[$t]}" >"$c/tools/$t"
        chmod +x "$c/tools/$t"
    done
    # fake setarch: logs every call; --version prints a version; `x86_64 -R cmd...` runs cmd (no personality change)
    cat >"$c/bin/setarch" <<EOF
#!${real[sh]}
echo "\$*" >>"$c/setarch.log"
f=\$(cat "$c/fault" 2>/dev/null || true)
case "\$1" in
--version) [ "\$f" = version ] && exit 1; echo "setarch (fake) 1.0"; exit 0 ;;
x86_64) [ "\$f" = personality ] && exit 1; shift 2; exec "\$@" ;;
esac
exit 64
EOF
    # fake readlink / sha256sum: fail on their fault, else the real tool
    cat >"$c/bin/readlink" <<EOF
#!${real[sh]}
[ "\$(cat "$c/fault" 2>/dev/null || true)" = realpath ] && exit 1
exec "${real[readlink]}" "\$@"
EOF
    cat >"$c/bin/sha256sum" <<EOF
#!${real[sh]}
[ "\$(cat "$c/fault" 2>/dev/null || true)" = sha256 ] && exit 1
exec "${real[sha256sum]}" "\$@"
EOF
    # fake cc: writes a fake child for -o OUT: the probe (exit 0), the positive control (one fake TSan data race report
    # naming writer_a / writer_b at the real RACE-A / RACE-B lines, exit 66) or the negative control (exit 0)
    cat >"$c/bin/cc" <<EOF
#!${real[sh]}
o= src= locked=0
while [ \$# -gt 0 ]; do
    case "\$1" in -o) o=\$2; shift ;; -DRACE_CTL_LOCKED) locked=1 ;; *.c) src=\$1 ;; esac
    shift
done
case "\$src" in
*tsan_race_ctl.c)
    if [ \$locked = 1 ]; then kind=negative; else kind=positive; fi
    la=\$(awk 'index(\$0, "/* RACE-A */") {print NR}' "\$src"); lb=\$(awk 'index(\$0, "/* RACE-B */") {print NR}' "\$src") ;;
*) kind=probe la= lb= ;;
esac
{
    echo "#!${real[sh]}"
    echo "echo \"\$kind TSAN_OPTIONS=\\\$TSAN_OPTIONS\" >>'$c/launch.log'"
    if [ \$kind = positive ]; then
        echo "echo '==================' >&2"
        echo "echo 'WARNING: ThreadSanitizer: data race (pid=1)' >&2"
        echo "echo '    #0 writer_b /src/tsan_race_ctl.c:'\$lb' (positive+0x1)' >&2"
        echo "echo '    #0 writer_a /src/tsan_race_ctl.c:'\$la' (positive+0x2)' >&2"
        echo "exit 66"
    else
        echo "exit 0"
    fi
} >"\$o"
chmod +x "\$o"
EOF
    printf '#!%s\nexit 7\n' "${real[sh]}" >"$c/bin/exit7"
    chmod +x "$c/bin/setarch" "$c/bin/readlink" "$c/bin/sha256sum" "$c/bin/cc" "$c/bin/exit7"
}

npass=0 nfail=0
want_tsan="halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=1:exitcode=66"
# check <case> <expected RESULT line> <description> [launch-log expectation] [setarch-log expectation]
#   launch: the exact launch.log kinds in order ("" = no child ran); setarch: the exact setarch.log lines
check() {
    local name="$1" want="$2" desc="$3" wantl="${4-}" wants="${5-__any__}" c="$W/$1" got gotl gots rc=0 why=""
    mkcase "$name"
    "${real[bash]}" "$here/driver-tsan-selftest.sh" --case "$name" "$c" >"$c/case.out" 2>"$c/case.err" || rc=$?
    got="$(grep '^RESULT ' "$c/case.out" || true)"
    gotl="$(cut -d' ' -f1 "$c/launch.log" 2>/dev/null | tr '\n' ' ' | sed 's/ $//' || true)"
    gots="$(tr '\n' '|' 2>/dev/null <"$c/setarch.log" | sed 's/|$//' || true)"
    [ "$rc" = 0 ] || why="case exit $rc;"
    [ "$got" = "RESULT $want" ] || why="$why result [${got#RESULT }];"
    [ "$gotl" = "$wantl" ] || why="$why launches [$gotl] want [$wantl];"
    if [ "$wants" != __any__ ] && [ "$gots" != "$wants" ]; then why="$why setarch calls [$gots] want [$wants];"; fi
    if [ -s "$c/launch.log" ] && grep -qv "TSAN_OPTIONS=$want_tsan\$" "$c/launch.log"; then
        why="$why a child ran without the declared TSAN_OPTIONS;"
    fi
    if [ -z "$why" ]; then
        npass=$((npass + 1))
        printf 'PASS  %-24s %s\n' "$name" "$desc"
    else
        nfail=$((nfail + 1))
        printf 'FAIL  %-24s %s: %s\n' "$name" "$desc" "$why"
    fi
}

check clean "admitted=1 controls=1 recfail=0 fail=0 notbuilt=" \
    "clean control: admitted, both controls PASS, every child saw the declared TSAN_OPTIONS" \
    "probe positive negative"
# probe launch records (sanitizer_links through admit_variant)
check probe-argv "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (probe-tsan/)" \
    "probe argv record fails: probe NOT launched, recorder FAIL" ""
check probe-env "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (probe-tsan/)" \
    "probe env record fails: probe NOT launched, recorder FAIL" ""
check probe-stdout "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (probe-tsan/)" \
    "probe stdout cannot be opened: probe NOT launched, recorder FAIL" ""
check probe-stderr "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (probe-tsan/)" \
    "probe stderr cannot be opened: probe NOT launched, recorder FAIL" ""
check probe-rc "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (probe-tsan/)" \
    "probe rc record fails after the launch: procedure FAILS" "probe"
# control launch records (tsan_controls through admit_tsan_controls)
check ctl-pos-argv "admitted=1 controls=0 recfail=1 fail=1 notbuilt=" \
    "positive argv record fails: positive and negative NOT launched" "probe"
check ctl-pos-env "admitted=1 controls=0 recfail=1 fail=1 notbuilt=" \
    "positive env record fails: positive and negative NOT launched" "probe"
check ctl-pos-rc "admitted=1 controls=0 recfail=1 fail=1 notbuilt=" \
    "positive rc record fails after its launch: FAIL, negative NOT launched" "probe positive"
check ctl-neg-argv "admitted=1 controls=0 recfail=1 fail=1 notbuilt=" \
    "negative argv record fails: negative NOT launched, FAIL" "probe positive"
check ctl-tsv "admitted=1 controls=0 recfail=1 fail=1 notbuilt=" \
    "controls.tsv cannot be written: FAIL even though the positive control detected the race" "probe positive"
# setarch identity (tsan_wrapper_ok through admit_variant): stop on the first failure, no personality call after it
unavail="setarch wrapper unavailable (setarch/)"
check id-commandv "admitted=0 controls=NA recfail=0 fail=1 notbuilt=$unavail" \
    "command -v finds no setarch: FAIL, no setarch call" "" ""
check id-realpath "admitted=0 controls=NA recfail=0 fail=1 notbuilt=$unavail" \
    "readlink -f fails: FAIL, stops before --version and the personality call" "" ""
check id-sha256 "admitted=0 controls=NA recfail=0 fail=1 notbuilt=$unavail" \
    "sha256sum fails: FAIL, stops before --version and the personality call" "" ""
check id-version "admitted=0 controls=NA recfail=0 fail=1 notbuilt=$unavail" \
    "setarch --version fails: FAIL, no personality call" "" "--version"
check id-personality "admitted=0 controls=NA recfail=0 fail=1 notbuilt=$unavail" \
    "personality call refused: FAIL" "" "--version|x86_64 -R true"
check id-write "admitted=0 controls=NA recfail=1 fail=1 notbuilt=recorder failure (setarch/)" \
    "setarch identity evidence write fails: recorder FAIL, no setarch call" "" ""
# exact status codes
check xrun-clean "st=0 xrc=7 recfail=0" "xrun records a child exit 7 as status 0, xrc 7 (recorder status separate from the child exit)" ""
check xrun-argv "st=3 xrc=NOT-RUN recfail=1" "xrun with the argv record a directory (codex 01:40 repro): status 3, child NOT run" "" ""
check xrun-rc "st=4 xrc=7 recfail=1" "xrun with the rc record a directory: status 4 after the child ran" ""
check direct-wrapper-ok "st=0 recfail=0" "tsan_wrapper_ok clean: 0" "" "--version|x86_64 -R true"
check direct-probe-argv "st=2 recfail=1" "sanitizer_links tsan with the argv record a directory: 2, probe NOT launched" ""
check direct-controls-rc "st=2 recfail=1" "tsan_controls with positive.rc a directory: 2" "positive"

echo "driver-tsan selftest: $npass passed, $nfail failed"
[ "$nfail" -eq 0 ]

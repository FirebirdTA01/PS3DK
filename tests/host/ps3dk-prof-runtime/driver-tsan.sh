# Sourced by build-and-run.sh and by driver-tsan-selftest.sh (never run on its own): the checked process recorder, the
# TSan launch wrapper, the setarch identity check, the sanitizer probes, the TSan detection controls and the variant
# admission that calls them.  Uses the caller's globals: out, here, CC, warn, vflags, controls, notbuilt (assoc),
# REQUIRE_SANITIZERS, and the ok / bad / skp reporters.
#
# Nothing here relies on errexit.  These functions run inside conditional contexts (`if ! admit_variant`, `|| r=$?`),
# where bash disables errexit for the whole function body, so every evidence write is checked explicitly and every
# function returns its recorder status separately from the child's exit status:
#
#   rrun / xrun          0 recorded (the child's exit is in rrc / xrc); 3 a pre-launch record failed (the child was NOT
#                        run, rrc / xrc = NOT-RUN); 4 a post-launch record failed (the child ran, its exit is in rrc / xrc,
#                        the rc file was not written)
#   tsan_wrapper_ok      0 usable; 1 setarch unavailable (the first failing identity command, or the personality call,
#                        in setarch/stopped.txt); 2 recorder failure
#   sanitizer_links      0 the probe built and ran clean; 1 the probe failed; 2 recorder failure
#   tsan_controls        0 both controls PASS; 1 a control failed; 2 recorder failure
#   admit_variant        0 admitted to the build; 1 not admitted (reported, notbuilt[v] set)
#   admit_tsan_controls  0 detection proven; 1 not proven or not recorded (reported)
#
# A recorder failure is always a FAIL (bad, and recfail counts it), whatever REQUIRE_SANITIZERS says.

recfail=0
# recorder <what>: count and report one evidence write or read that failed.
recorder() {
    recfail=$((recfail + 1))
    echo "RECORDER FAILURE: $*" >&2
}
# put <file> <text> / add <file> <text>: write / append <text> and a newline, checked.  mkd <dir>: mkdir -p, checked.
put() { if printf '%s\n' "$2" >"$1"; then return 0; fi; recorder "cannot write $1"; return 1; }
add() { if printf '%s\n' "$2" >>"$1"; then return 0; fi; recorder "cannot append to $1"; return 1; }
mkd() { if mkdir -p "$1"; then return 0; fi; recorder "cannot create $1"; return 1; }

# The declared TSan options, exported before any TSan-instrumented executable starts (the startup probe included), so
# the probe, both controls and every test see the same options.  exitcode is declared: the positive control must exit
# with exactly this value.  No effect on the plain and asan variants.
tsan_exitcode=66
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=1:exitcode=$tsan_exitcode"

# rrun <stdout> <stderr> <rc-file> <cmd...>: run <cmd...> with both streams and its exit recorded.  Both stream files are
# opened BEFORE the child starts, so a stream that cannot be opened prevents the launch (status 3) instead of reading as
# a child exit.
rrc=NOT-RUN
rrun() {
    local so="$1" se="$2" rf="$3" fo fe c=0
    shift 3
    rrc=NOT-RUN
    if ! exec {fo}>"$so"; then
        recorder "cannot open $so; $1 not run"
        return 3
    fi
    if ! exec {fe}>"$se"; then
        exec {fo}>&-
        recorder "cannot open $se; $1 not run"
        return 3
    fi
    "$@" >&"$fo" 2>&"$fe" {fo}>&- {fe}>&- || c=$?
    exec {fo}>&- {fe}>&-
    rrc=$c
    if ! printf '%s\n' "$c" >"$rf"; then
        recorder "cannot write $rf ($1 exited $c)"
        return 4
    fi
    return 0
}

# The TSan launch wrapper.  xrun <prefix> <stdout> <stderr> <rc-file> <timeout-s> <cmd...> runs
# `timeout <s> <setarch> x86_64 -R <cmd...>` and keeps <prefix>.argv (the argv as executed, one argument per line),
# <prefix>.env (the environment), both streams and the rc; the child's exit is left in xrc.  It is the only way a
# TSan-instrumented executable is started.  The argv and env records are written before the launch: if either fails,
# the child is not started.
setarch=""
xrc=NOT-RUN
xrun() {
    local p="$1" so="$2" se="$3" rf="$4" t="$5" r=0
    shift 5
    local argv=(timeout "$t" "$setarch" x86_64 -R "$@")
    xrc=NOT-RUN
    if ! printf '%s\n' "${argv[@]}" >"$p.argv"; then
        recorder "cannot write $p.argv; not launched"
        return 3
    fi
    if ! env >"$p.env"; then
        recorder "cannot write $p.env; not launched"
        return 3
    fi
    rrun "$so" "$se" "$rf" "${argv[@]}" || r=$?
    xrc=$rrc
    return "$r"
}

# setarch for the TSan wrapper.  Each identity command runs in turn and is recorded in setarch/ (<step>.txt / .err /
# .rc): `command -v setarch`, `readlink -f`, `sha256sum`, `setarch --version`, then the personality call itself
# (`setarch x86_64 -R true`).  The FIRST failure stops the check, so a working personality call never hides a failed
# identity command.  A command that exits 0 with no output is a failure too.
tsan_wrapper_ok() {
    local dir="$out/setarch" step
    setarch=""
    mkd "$dir" || return 2
    rrun "$dir/command-v.txt" "$dir/command-v.err" "$dir/command-v.rc" command -v setarch || return 2
    if [ "$rrc" != 0 ] || [ ! -s "$dir/command-v.txt" ]; then
        put "$dir/stopped.txt" "command -v setarch: exit $rrc, no path" || return 2
        return 1
    fi
    if ! IFS= read -r setarch <"$dir/command-v.txt" || [ -z "$setarch" ]; then
        recorder "cannot read $dir/command-v.txt"
        setarch=""
        return 2
    fi
    for step in realpath sha256 version; do
        case "$step" in
        realpath) set -- readlink -f "$setarch" ;;
        sha256) set -- sha256sum "$setarch" ;;
        version) set -- "$setarch" --version ;;
        esac
        rrun "$dir/$step.txt" "$dir/$step.err" "$dir/$step.rc" "$@" || return 2
        if [ "$rrc" != 0 ] || [ ! -s "$dir/$step.txt" ]; then
            put "$dir/stopped.txt" "$step: $* exit $rrc$([ -s "$dir/$step.txt" ] || echo ', no output')" || return 2
            return 1
        fi
    done
    rrun "$dir/personality-check.txt" "$dir/personality-check.err" "$dir/personality-check.rc" \
        "$setarch" x86_64 -R true || return 2
    if [ "$rrc" != 0 ]; then
        put "$dir/stopped.txt" "personality: $setarch x86_64 -R true exit $rrc" || return 2
        return 1
    fi
    return 0
}

# The probe keeps its compile and run exit codes and streams in probe-<v>/ (compile.log/.err/.rc, run.log/.err/.rc; the
# tsan probe runs through xrun: run.argv/.env/.stdout/.stderr/.rc).
sanitizer_links() {
    local v="$1"
    local dir="$out/probe-$v" extra   # a separate local: in one `local`, $v would expand before v is set
    if ! read -r -a extra <<<"${vflags[$v]}"; then
        recorder "cannot read the $v flags"
        return 2
    fi
    mkd "$dir" || return 2
    put "$dir/p.c" 'int main(void){return 0;}' || return 2
    rrun "$dir/compile.log" "$dir/compile.err" "$dir/compile.rc" "$CC" "${extra[@]}" "$dir/p.c" -o "$dir/p" || return 2
    [ "$rrc" = 0 ] || return 1
    if [ "$v" = tsan ]; then
        xrun "$dir/run" "$dir/run.stdout" "$dir/run.stderr" "$dir/run.rc" 30 "$dir/p" || return 2
        [ "$xrc" = 0 ] || return 1
        return 0
    fi
    rrun "$dir/run.log" "$dir/run.err" "$dir/run.rc" "$dir/p" || return 2
    [ "$rrc" = 0 ] || return 1
    return 0
}

# control <name> <result> <reason>: one controls.tsv row, checked.
control() { add "$controls" "$1"$'\t'"$2"$'\t'"$3"; }

# The racing lines are frozen in tsan_race_ctl.c by their markers: exactly one line each.
marker_line() {
    local n
    n="$(awk -v m="$1" 'index($0, m) {print NR}' "$here/tsan_race_ctl.c")" || return 1
    case "$n" in '' | *[!0-9]*) return 1 ;; esac
    echo "$n"
}

# grep_status <grep args...>: grep with its exit kept: 0 match, 1 no match, >1 the file could not be read (recorded).
gst=0
grep_status() {
    gst=0
    grep "$@" || gst=$?
    if [ "$gst" -gt 1 ]; then
        recorder "grep $* failed (exit $gst)"
        return 2
    fi
    return 0
}

# tsan_controls: build both controls with the tsan compiler and flags, run them through xrun, judge them.  Each control's
# exit is captured into its own variable as soon as xrun returns.
tsan_controls() {
    local dir="$out/controls-tsan" extra la lb c n prc nrc
    if ! read -r -a extra <<<"${vflags[tsan]}"; then
        recorder "cannot read the tsan flags"
        return 2
    fi
    mkd "$dir" || return 2
    if ! la="$(marker_line '/* RACE-A */')" || ! lb="$(marker_line '/* RACE-B */')"; then
        control markers FAIL "tsan_race_ctl.c must hold exactly one RACE-A and one RACE-B line" || return 2
        return 1
    fi
    put "$dir/race-lines.txt" "RACE-A $la"$'\n'"RACE-B $lb" || return 2
    for c in positive negative; do
        local defs=()
        if [ "$c" = negative ]; then
            defs=(-DRACE_CTL_LOCKED)
        fi
        rrun "$dir/$c.compile.log" "$dir/$c.compile.err" "$dir/$c.compile.rc" "$CC" -std=c11 -O1 -g -pthread \
            "${warn[@]}" "${extra[@]}" "${defs[@]}" "$here/tsan_race_ctl.c" -o "$dir/$c" || return 2
        if [ "$rrc" != 0 ]; then
            control "$c" FAIL "compile exit $rrc ($dir/$c.compile.err)" || return 2
            return 1
        fi
    done

    xrun "$dir/positive" "$dir/positive.stdout" "$dir/positive.stderr" "$dir/positive.rc" 30 "$dir/positive" || return 2
    prc=$xrc
    grep_status -c 'WARNING: ThreadSanitizer' "$dir/positive.stderr" >"$dir/positive.reports" || return 2
    if ! IFS= read -r n <"$dir/positive.reports"; then
        recorder "cannot read $dir/positive.reports"
        return 2
    fi
    if [ "$prc" != "$tsan_exitcode" ]; then
        control positive FAIL "exit $prc, want the declared exitcode $tsan_exitcode" || return 2
        return 1
    fi
    grep_status -q 'FATAL: ThreadSanitizer' "$dir/positive.stderr" || return 2
    if [ "$gst" = 0 ]; then
        control positive FAIL "ThreadSanitizer FATAL (startup), not a detection" || return 2
        return 1
    fi
    grep_status -q 'WARNING: ThreadSanitizer: data race' "$dir/positive.stderr" || return 2
    if [ "$n" != 1 ] || [ "$gst" != 0 ]; then
        control positive FAIL "want exactly one ThreadSanitizer report, a data race; got $n reports" || return 2
        return 1
    fi
    grep_status -qE " writer_a .*tsan_race_ctl\.c:$la([^0-9]|\$)" "$dir/positive.stderr" || return 2
    local ga=$gst
    grep_status -qE " writer_b .*tsan_race_ctl\.c:$lb([^0-9]|\$)" "$dir/positive.stderr" || return 2
    if [ "$ga" != 0 ] || [ "$gst" != 0 ]; then
        control positive FAIL "the race report does not name writer_a at line $la and writer_b at line $lb" || return 2
        return 1
    fi
    control positive PASS "exit $prc, one data race: writer_a line $la, writer_b line $lb" || return 2

    xrun "$dir/negative" "$dir/negative.stdout" "$dir/negative.stderr" "$dir/negative.rc" 30 "$dir/negative" || return 2
    nrc=$xrc
    if [ "$nrc" != 0 ]; then
        control negative FAIL "exit $nrc, want 0" || return 2
        return 1
    fi
    grep_status -qE 'ThreadSanitizer|AddressSanitizer|LeakSanitizer|runtime error' "$dir/negative.stderr" || return 2
    if [ "$gst" = 0 ]; then
        control negative FAIL "sanitizer output on stderr" || return 2
        return 1
    fi
    control negative PASS "exit 0, no sanitizer report" || return 2
    return 0
}

# admit_variant <v>: the gate before building <v>: for tsan the setarch wrapper, then for asan and tsan the sanitizer
# probe.  The driver calls it as `if ! admit_variant "$v"; then continue; fi`.
admit_variant() {
    local v="$1" r=0
    if [ "$v" = tsan ]; then
        tsan_wrapper_ok || r=$?
        if [ "$r" = 2 ]; then
            bad "build tsan: recorder failure while checking setarch (setarch/)"
            notbuilt[$v]="recorder failure (setarch/)"
            return 1
        elif [ "$r" != 0 ]; then
            bad "build tsan: setarch is missing, an identity command failed or the ADDR_NO_RANDOMIZE personality was refused (setarch/stopped.txt)"
            notbuilt[$v]="setarch wrapper unavailable (setarch/)"
            return 1
        fi
    fi
    if [ "$v" = plain ]; then
        return 0
    fi
    r=0
    sanitizer_links "$v" || r=$?
    if [ "$r" = 2 ]; then
        bad "build $v: recorder failure in the sanitizer probe (probe-$v/)"
        notbuilt[$v]="recorder failure (probe-$v/)"
        return 1
    elif [ "$r" != 0 ]; then
        if [ "${REQUIRE_SANITIZERS:-0}" = 1 ]; then
            bad "build $v: the compiler cannot build and run $v programs (probe-$v/)"
        else
            skp "build $v: the compiler cannot build and run $v programs (probe-$v/)"
        fi
        notbuilt[$v]="sanitizer probe failed (probe-$v/)"
        return 1
    fi
    return 0
}

# admit_tsan_controls: run the detection controls; the driver calls it as `if ! admit_tsan_controls; then ...`.
admit_tsan_controls() {
    local r=0
    tsan_controls || r=$?
    if [ "$r" = 0 ]; then
        ok "tsan detection controls (controls.tsv)"
        return 0
    elif [ "$r" = 2 ]; then
        bad "tsan detection controls: recorder failure ($controls, $out/controls-tsan/)"
    else
        bad "tsan detection controls ($controls, $out/controls-tsan/)"
    fi
    return 1
}

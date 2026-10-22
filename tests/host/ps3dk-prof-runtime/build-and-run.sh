#!/usr/bin/env bash
# Host build and run of the ps3dk-prof runtime B tests (step 3, host half).
#
#   tests/host/ps3dk-prof-runtime/build-and-run.sh <out-dir>
#
# With the HOST compiler only (CC, CXX; default cc and c++), into <out-dir>:
#   1. builds the portable core + the host PAL + the tests in three variants: plain, asan (ASan + UBSan) and tsan.
#      A sanitizer the compiler cannot link is SKIPped; REQUIRE_SANITIZERS=1 makes that a failure;
#      Each probe keeps its compile/run exit codes and diagnostics under probe-<v>/;
#   2. runs every rt_* test in its own process with a fresh output directory.  The names come from the frozen
#      rt-registry.txt; each built variant's `rt --list` (exit code checked) must equal it exactly, else that variant
#      FAILs and its tests are NOT RUN.  results.tsv holds one line per variant x registry test (PASS, FAIL or NOT RUN
#      with a reason; a variant that could not be built or probed is NOT RUN), counts.txt the totals;
#   3. checks every capture a test names ("CAPTURE <path> EXPECT <n>") with the Rust reader,
#      `cargo run -p ps3dk-prof -- check <path>` (READER=<binary> uses a prebuilt reader instead), and compares the exit;
#   4. runs the host off-build checks: OB-E1 (C) and OB-E2 (C++) built enabled, undefined and =0 and RUN; host OB-P
#      (preprocessed disabled source); OB-N (no profiler symbol referenced by the disabled objects); PROBE-G (the C++
#      guard is not copyable or movable); and the archive-content check (the hook-free objects export exactly the API
#      under ps3dk_prof_, internals only under ps3dk_prof__, no ps3dk_prof_test_ symbol, no release function).
# TSan: every TSan-instrumented executable (the startup probe, both detection controls, `rt --list` and every test) is
# launched as `timeout N setarch x86_64 -R <cmd>` (process-local ADDR_NO_RANDOMIZE; no global setting is changed), and
# each launch keeps its argv as executed, its environment, rc, stdout and stderr.  A missing setarch or a refused
# personality is a FAIL with the tsan rows NOT RUN, never an ordinary launch.  Before the tsan tests, two controls from
# tsan_race_ctl.c, built with the tsan compiler and flags (controls.tsv, controls-tsan/): the POSITIVE control must exit
# with the declared TSAN_OPTIONS exitcode AND print exactly one ThreadSanitizer report, a data race whose stacks name
# writer_a at its RACE-A line and writer_b at its RACE-B line; the NEGATIVE control (mutex) must exit 0 with no
# sanitizer report.  A failed control fails the variant and its 97 rows are NOT RUN (detection not proven).
# Plain and asan launches are unchanged.
# Exits non-zero on any failure.  Never runs a cross compiler, make, cmake or wsl.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
if [ $# -ne 1 ]; then
    echo "usage: $0 <out-dir>" >&2
    exit 2
fi
mkdir -p "$1"
out="$(cd "$1" && pwd)"

CC="${CC:-cc}"
CXX="${CXX:-c++}"
inc=(-I"$root/sdk/include" -I"$root/sdk/libps3dkprof/src/core" -I"$root/sdk/libps3dkprof/src/pal")
warn=(-Wall -Wextra -Werror)
lib_src=(prof_core prof_drain pal_host)
test_c=(rt_main rt_util rt_t_lifecycle rt_t_stop rt_t_zones rt_t_init rt_t_sink rt_t_race rt_t_counters)

src_of() {
    case "$1" in
    pal_host) echo "$root/sdk/libps3dkprof/src/pal/pal_host.c" ;;
    prof_*) echo "$root/sdk/libps3dkprof/src/core/$1.c" ;;
    *) echo "$here/$1.c" ;;
    esac
}

pass=0
fail=0
skip=0
ok() { pass=$((pass + 1)); echo "PASS $*"; }
bad() { fail=$((fail + 1)); echo "FAIL $*"; }
skp() { skip=$((skip + 1)); echo "SKIP $*"; }

# clang warns about side effects inside sizeof (-Wunevaluated-expression); unevaluated side effects are the point here.
noeval() { local v; v="$("$1" --version 2>&1 || true)"; case "$v" in *[Cc]lang*) echo -Wno-unevaluated-expression ;; esac; }
cc_noeval="$(noeval "$CC")"
cxx_noeval="$(noeval "$CXX")"

if [ -n "${READER:-}" ]; then
    reader=("$READER" check)
else
    echo "== building the Rust reader"
    cargo build -q --manifest-path "$root/tools/Cargo.toml" -p ps3dk-prof
    reader=(cargo run -q --manifest-path "$root/tools/Cargo.toml" -p ps3dk-prof -- check)
fi

# check_capture <path> <want> <label>
check_capture() {
    local got=0 txt="$1.check.txt"
    "${reader[@]}" "$1" >"$txt" 2>&1 || got=$?
    echo "$got" >"$1.check.rc"
    if [ "$got" = "$2" ]; then
        return 0
    fi
    bad "$3: reader exit $got, want $2: $(head -n 1 "$txt")"
    return 1
}

# ---- 1. build ---------------------------------------------------------------------------------------------------------

declare -A vflags=([plain]="" [asan]="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" [tsan]="-fsanitize=thread")
variants=()

build_variant() {
    local v="$1" dir="$out/build-$v" s o
    local extra
    read -r -a extra <<<"${vflags[$v]}"
    local cflags=(-std=c11 -O1 -g -pthread -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS "${inc[@]}" "${warn[@]}" "${extra[@]}")
    local objs=()
    mkdir -p "$dir"
    for s in "${lib_src[@]}" "${test_c[@]}"; do
        o="$dir/$s.o"
        "$CC" "${cflags[@]}" -c "$(src_of "$s")" -o "$o" 2>"$dir/$s.log" || { cat "$dir/$s.log"; return 1; }
        objs+=("$o")
    done
    "$CXX" -std=c++17 -O1 -g -pthread -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS "${inc[@]}" \
        "${warn[@]}" "${extra[@]}" -c "$here/rt_cxx.cpp" -o "$dir/rt_cxx.o" 2>"$dir/rt_cxx.log" ||
        { cat "$dir/rt_cxx.log"; return 1; }
    "$CXX" -pthread "${extra[@]}" "${objs[@]}" "$dir/rt_cxx.o" -o "$dir/rt" 2>"$dir/link.log" ||
        { cat "$dir/link.log"; return 1; }
}

# The TSan launch wrapper.  xrun <prefix> <stdout> <stderr> <rc-file> <timeout-s> <cmd...> runs
# `timeout <s> <setarch> x86_64 -R <cmd...>` and keeps <prefix>.argv (the argv as executed, one argument per line),
# <prefix>.env (the environment), the rc and both streams; the rc is also left in xrc.  It is the only way a
# TSan-instrumented executable is started.
setarch=""
xrc=0
xrun() {
    local p="$1" so="$2" se="$3" rf="$4" t="$5"
    shift 5
    local argv=(timeout "$t" "$setarch" x86_64 -R "$@")
    printf '%s\n' "${argv[@]}" >"$p.argv"
    env >"$p.env"
    xrc=0
    "${argv[@]}" >"$so" 2>"$se" || xrc=$?
    echo "$xrc" >"$rf"
}

# setarch for the TSan wrapper: resolved with `command -v`, its path, realpath, version and digest recorded in
# setarch/, and the personality call itself checked once (`setarch x86_64 -R true`).  Any failure: no TSan launch.
tsan_wrapper_ok() {
    local dir="$out/setarch" rc=0
    mkdir -p "$dir"
    setarch="$(command -v setarch || true)"
    printf '%s\n' "${setarch:-MISSING}" >"$dir/path.txt"
    [ -n "$setarch" ] || return 1
    readlink -f "$setarch" >"$dir/realpath.txt" 2>&1 || rc=$?
    echo "$rc" >"$dir/realpath.rc"
    rc=0
    sha256sum "$setarch" >"$dir/sha256.txt" 2>&1 || rc=$?
    echo "$rc" >"$dir/sha256.rc"
    rc=0
    "$setarch" --version >"$dir/version.txt" 2>&1 || rc=$?
    echo "$rc" >"$dir/version.rc"
    rc=0
    "$setarch" x86_64 -R true >"$dir/personality-check.log" 2>&1 || rc=$?
    echo "$rc" >"$dir/personality-check.rc"
    [ "$rc" = 0 ]
}

# The probe keeps its compile and run exit codes and diagnostics in probe-<v>/ (compile.rc/.log, run.rc/.log; the tsan
# probe runs through xrun: run.argv/.env/.stdout/.stderr/.rc).
sanitizer_links() {
    local v="$1" dir="$out/probe-$v" extra rc=0
    read -r -a extra <<<"${vflags[$v]}"
    mkdir -p "$dir"
    printf 'int main(void){return 0;}\n' >"$dir/p.c"
    "$CC" "${extra[@]}" "$dir/p.c" -o "$dir/p" >"$dir/compile.log" 2>&1 || rc=$?
    echo "$rc" >"$dir/compile.rc"
    [ "$rc" = 0 ] || return 1
    if [ "$v" = tsan ]; then
        xrun "$dir/run" "$dir/run.stdout" "$dir/run.stderr" "$dir/run.rc" 30 "$dir/p"
        [ "$xrc" = 0 ]
        return
    fi
    "$dir/p" >"$dir/run.log" 2>&1 || rc=$?
    echo "$rc" >"$dir/run.rc"
    [ "$rc" = 0 ]
}

# Why each variant's tests did not run, when they did not ("" = built).
declare -A notbuilt=()
for v in plain asan tsan; do
    if [ "$v" = tsan ] && ! tsan_wrapper_ok; then
        bad "build tsan: setarch is missing or the ADDR_NO_RANDOMIZE personality was refused (setarch/)"
        notbuilt[$v]="setarch wrapper unavailable (setarch/)"
        continue
    fi
    if [ "$v" != plain ] && ! sanitizer_links "$v"; then
        if [ "${REQUIRE_SANITIZERS:-0}" = 1 ]; then
            bad "build $v: the compiler cannot build and run $v programs (probe-$v/)"
        else
            skp "build $v: the compiler cannot build and run $v programs (probe-$v/)"
        fi
        notbuilt[$v]="sanitizer probe failed (probe-$v/)"
        continue
    fi
    echo "== building $v"
    rc=0
    build_variant "$v" || rc=$?
    echo "$rc" >"$out/build-$v.rc"
    if [ "$rc" = 0 ]; then
        ok "build $v"
        variants+=("$v")
    else
        bad "build $v"
        notbuilt[$v]="build failed (build-$v/)"
    fi
done

# ---- 2 + 3. run every test and check its captures --------------------------------------------------------------------

export ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
# exitcode is declared explicitly: the positive TSan control must exit with exactly this value.
tsan_exitcode=66
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=1:exitcode=$tsan_exitcode"

# controls.tsv: the TSan detection controls (separate from results.tsv): control, result, reason.
controls="$out/controls.tsv"
printf 'control\tresult\treason\n' >"$controls"
control() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$controls"; }

# The racing lines are frozen in tsan_race_ctl.c by their markers: exactly one line each.
marker_line() {
    local n
    n="$(awk -v m="$1" 'index($0, m) {print NR}' "$here/tsan_race_ctl.c")"
    case "$n" in '' | *[!0-9]*) return 1 ;; esac
    echo "$n"
}

# tsan_controls: build both controls with the tsan compiler and flags, run them through xrun, judge them.
tsan_controls() {
    local dir="$out/controls-tsan" extra la lb rc=0 n c
    read -r -a extra <<<"${vflags[tsan]}"
    mkdir -p "$dir"
    if ! la="$(marker_line '/* RACE-A */')" || ! lb="$(marker_line '/* RACE-B */')"; then
        control markers FAIL "tsan_race_ctl.c must hold exactly one RACE-A and one RACE-B line"
        return 1
    fi
    printf 'RACE-A %s\nRACE-B %s\n' "$la" "$lb" >"$dir/race-lines.txt"
    for c in positive negative; do
        local defs=()
        [ "$c" = negative ] && defs=(-DRACE_CTL_LOCKED)
        rc=0
        "$CC" -std=c11 -O1 -g -pthread "${warn[@]}" "${extra[@]}" "${defs[@]}" "$here/tsan_race_ctl.c" -o "$dir/$c" \
            >"$dir/$c.compile.log" 2>&1 || rc=$?
        echo "$rc" >"$dir/$c.compile.rc"
        if [ "$rc" != 0 ]; then
            control "$c" FAIL "compile exit $rc ($dir/$c.compile.log)"
            return 1
        fi
    done
    xrun "$dir/positive" "$dir/positive.stdout" "$dir/positive.stderr" "$dir/positive.rc" 30 "$dir/positive"
    n="$(grep -c 'WARNING: ThreadSanitizer' "$dir/positive.stderr" || true)"
    if [ "$xrc" != "$tsan_exitcode" ]; then
        control positive FAIL "exit $xrc, want the declared exitcode $tsan_exitcode"
        return 1
    elif grep -q 'FATAL: ThreadSanitizer' "$dir/positive.stderr"; then
        control positive FAIL "ThreadSanitizer FATAL (startup), not a detection"
        return 1
    elif [ "$n" != 1 ] || ! grep -q 'WARNING: ThreadSanitizer: data race' "$dir/positive.stderr"; then
        control positive FAIL "want exactly one ThreadSanitizer report, a data race; got $n reports"
        return 1
    elif ! grep -qE " writer_a .*tsan_race_ctl\.c:$la([^0-9]|\$)" "$dir/positive.stderr" ||
        ! grep -qE " writer_b .*tsan_race_ctl\.c:$lb([^0-9]|\$)" "$dir/positive.stderr"; then
        control positive FAIL "the race report does not name writer_a at line $la and writer_b at line $lb"
        return 1
    fi
    control positive PASS "exit $xrc, one data race: writer_a line $la, writer_b line $lb"
    xrun "$dir/negative" "$dir/negative.stdout" "$dir/negative.stderr" "$dir/negative.rc" 30 "$dir/negative"
    if [ "$xrc" != 0 ]; then
        control negative FAIL "exit $xrc, want 0"
        return 1
    elif grep -qE 'ThreadSanitizer|AddressSanitizer|LeakSanitizer|runtime error' "$dir/negative.stderr"; then
        control negative FAIL "sanitizer output on stderr"
        return 1
    fi
    control negative PASS "exit 0, no sanitizer report"
}

# The frozen registry: the 97 test names, sorted, unique.  Each built variant's `rt --list` must equal it exactly.
registry="$here/rt-registry.txt"
mapfile -t names <"$registry"
if [ "${#names[@]}" -eq 0 ] || [ "$(LC_ALL=C sort -u "$registry")" != "$(cat "$registry")" ]; then
    bad "registry $registry is empty, unsorted or not unique"
    names=()
fi

# results.tsv: one line per variant x registry test: variant, test, PASS | FAIL | NOT RUN, reason.
results="$out/results.tsv"
printf 'variant\ttest\tresult\treason\n' >"$results"
notrun=0
result() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$results"; }
not_run_all() {
    local t
    for t in "${names[@]}"; do
        result "$1" "$t" "NOT RUN" "$2"
        notrun=$((notrun + 1))
    done
}

for v in plain asan tsan; do
    if [ -n "${notbuilt[$v]:-}" ]; then
        not_run_all "$v" "${notbuilt[$v]}"
        continue
    fi
    if [ "$v" = tsan ]; then
        if tsan_controls; then
            ok "tsan detection controls (controls.tsv)"
        else
            bad "tsan detection controls ($controls, $out/controls-tsan/)"
            not_run_all "$v" "TSan detection not proven: a control failed (controls.tsv)"
            continue
        fi
    fi
    bin="$out/build-$v/rt"
    mkdir -p "$out/run-$v"
    lst="$out/run-$v/list.txt"
    rc=0
    if [ "$v" = tsan ]; then
        xrun "$out/run-$v/list" "$lst" "$out/run-$v/list.stderr" "$out/run-$v/list.rc" 60 "$bin" --list
        rc=$xrc
    else
        "$bin" --list >"$lst" 2>"$out/run-$v/list.stderr" || rc=$?
        echo "$rc" >"$out/run-$v/list.rc"
    fi
    if [ "$rc" != 0 ] || [ ! -s "$lst" ] || [ "$(LC_ALL=C sort -u "$lst")" != "$(cat "$registry")" ] ||
        [ "$(wc -l <"$lst")" != "${#names[@]}" ]; then
        bad "$v: rt --list (exit $rc) is empty, not unique or differs from rt-registry.txt ($lst)"
        not_run_all "$v" "rt --list failed or differs from the registry"
        continue
    fi
    for t in "${names[@]}"; do
        d="$out/run-$v/$t"
        rm -rf "$d"
        mkdir -p "$d"
        rc=0
        if [ "$v" = tsan ]; then
            xrun "$d/exec" "$d/stdout" "$d/stderr" "$d/rc" 180 "$bin" "$t" "$d"
            rc=$xrc
        else
            timeout 180 "$bin" "$t" "$d" >"$d/stdout" 2>"$d/stderr" || rc=$?
            echo "$rc" >"$d/rc"
        fi
        if [ "$rc" -ne 0 ]; then
            bad "$v $t: exit $rc ($d/stderr): $(grep -m 1 -E 'CHECK|ERROR|WARNING|runtime error' "$d/stderr" || true)"
            result "$v" "$t" FAIL "exit $rc"
            continue
        fi
        if grep -qE 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' "$d/stderr"; then
            bad "$v $t: sanitizer report ($d/stderr)"
            result "$v" "$t" FAIL "sanitizer report"
            continue
        fi
        caps_ok=1
        while read -r tag path tag2 want; do
            [ "$tag" = CAPTURE ] && [ "$tag2" = EXPECT ] || continue
            check_capture "$path" "$want" "$v $t" || caps_ok=0
        done <"$d/stdout"
        if [ "$caps_ok" = 1 ]; then
            ok "$v $t"
            result "$v" "$t" PASS ""
        else
            result "$v" "$t" FAIL "capture verdict"
        fi
    done
done

# ---- 4. off-build checks (host) --------------------------------------------------------------------------------------

ob="$out/offbuild"
rm -rf "$ob"
mkdir -p "$ob"
echo "== off-build checks"

# The library exactly as an SDK archive would hold it: no test hooks.
libobjs=()
lib_ok=1
for s in "${lib_src[@]}"; do
    if "$CC" -std=c11 -O1 -pthread -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST "${inc[@]}" "${warn[@]}" -c "$(src_of "$s")" \
        -o "$ob/lib-$s.o" 2>"$ob/lib-$s.log"; then
        libobjs+=("$ob/lib-$s.o")
    else
        cat "$ob/lib-$s.log"
        lib_ok=0
    fi
done
if [ "$lib_ok" = 1 ]; then
    ok "library build without test hooks"
else
    bad "library build without test hooks"
fi

if [ "$lib_ok" = 1 ]; then
    api="ps3dk_prof_config_defaults ps3dk_prof_drain ps3dk_prof_frame ps3dk_prof_frame_token ps3dk_prof_init \
ps3dk_prof_set_sink_callbacks ps3dk_prof_set_sink_path ps3dk_prof_shutdown ps3dk_prof_thread_begin \
ps3dk_prof_thread_end ps3dk_prof_zone_begin ps3dk_prof_zone_end"
    # nm output goes to files first: under pipefail a `nm | grep -q` can exit 141 on an early match.
    nm -g --defined-only "${libobjs[@]}" >"$ob/nm-defined.txt" || bad "nm --defined-only failed"
    nm "${libobjs[@]}" >"$ob/nm-all.txt" || bad "nm failed"
    defined="$(awk 'NF >= 3 {print $3}' "$ob/nm-defined.txt" | sort -u)"
    public="$(printf '%s\n' "$defined" | grep -E '^ps3dk_prof_[a-z]' | tr '\n' ' ' | sed 's/ $//')"
    other="$(printf '%s\n' "$defined" | grep -vE '^ps3dk_prof_' || true)"
    if [ "$public" = "$(echo $api)" ]; then
        ok "archive exports exactly the API (no release function)"
    else
        bad "archive exports: [$public], want [$(echo $api)]"
    fi
    if [ -z "$other" ]; then
        ok "archive internals are all under ps3dk_prof__"
    else
        bad "archive exports symbols outside ps3dk_prof_: $(echo $other)"
    fi
    if grep -q 'ps3dk_prof_test_' "$ob/nm-all.txt"; then
        bad "archive objects contain a ps3dk_prof_test_ symbol"
    else
        ok "archive objects contain no test-hook symbol"
    fi
fi

# OB-E1 (C) and OB-E2 (C++): the same source, enabled / undefined / =0.
ob_case() {
    local lang="$1" mode="$2" tag="ob-$1-$2" comp std noev defs=() o="$ob/ob-$1-$2"
    if [ "$lang" = c ]; then
        comp="$CC"; std=(-std=c11 -x c); noev="$cc_noeval"
    else
        comp="$CXX"; std=(-std=c++17 -x c++); noev="$cxx_noeval"
    fi
    case "$mode" in
    on) defs=(-DPS3DK_PROF=1) ;;
    undef) defs=() ;;
    zero) defs=(-DPS3DK_PROF=0) ;;
    esac
    # shellcheck disable=SC2086
    if ! "$comp" "${std[@]}" -O1 -pthread "${defs[@]}" -I"$root/sdk/include" "${warn[@]}" $noev \
        -c "$here/ob_same_source.c" -o "$o.o" 2>"$o.log"; then
        cat "$o.log"
        bad "$tag: compile (-Wall -Wextra -Werror)"
        return
    fi
    if [ "$mode" != on ]; then
        if ! nm -u "$o.o" >"$o.nm-u.txt"; then
            bad "$tag: OB-N: nm -u failed"
        elif grep -q 'ps3dk_prof' "$o.nm-u.txt"; then
            bad "$tag: OB-N: the disabled object references $(grep -m 1 ps3dk_prof "$o.nm-u.txt")"
        else
            ok "$tag: OB-N (no profiler symbol referenced)"
        fi
        # shellcheck disable=SC2086
        if "$comp" "${std[@]}" "${defs[@]}" -I"$root/sdk/include" $noev -E "$here/ob_same_source.c" -o "$o.i" &&
            python3 "$here/ob_pp_check.py" "$o.i" >"$o.pp.txt"; then
            ok "$tag: OB-P host (side effects only inside sizeof, no profiler function named)"
        else
            bad "$tag: OB-P host: $(tail -n 3 "$o.pp.txt" 2>/dev/null | tr '\n' ' ')"
        fi
        if ! "$comp" -pthread "$o.o" -o "$o.bin" 2>"$o.link.log"; then
            cat "$o.link.log"
            bad "$tag: link"
            return
        fi
        if "$o.bin" >"$o.run.txt" 2>&1 && grep -qx 'hits=0' "$o.run.txt"; then
            ok "$tag: run (hits == 0: no argument evaluated)"
        else
            bad "$tag: run: $(cat "$o.run.txt")"
        fi
        return
    fi
    if ! "$CXX" -pthread "$o.o" "${libobjs[@]}" -o "$o.bin" 2>"$o.link.log"; then
        cat "$o.link.log"
        bad "$tag: link with the library"
        return
    fi
    mkdir -p "$o.cap"
    if "$o.bin" "$o.cap" >"$o.run.txt" 2>&1 && grep -qx 'hits=4' "$o.run.txt"; then
        ok "$tag: run (hits == 4: each runtime argument evaluated once)"
    else
        bad "$tag: run: $(cat "$o.run.txt")"
        return
    fi
    local caps=("$o.cap"/*.prof)
    if [ "${#caps[@]}" -eq 1 ] && [ -f "${caps[0]}" ] && check_capture "${caps[0]}" 0 "$tag"; then
        ok "$tag: capture COMPLETE through the reader"
    elif [ "${#caps[@]}" -ne 1 ] || [ ! -f "${caps[0]}" ]; then
        bad "$tag: expected one capture in $o.cap"
    fi
}

if [ "$lib_ok" = 1 ]; then
    for lang in c cxx; do
        for mode in on undef zero; do
            ob_case "$lang" "$mode"
        done
    done
fi

# PROBE-G: the guard is not copyable and not movable.
probe() {
    "$CXX" -std=c++17 -fsyntax-only -I"$root/sdk/include" "${warn[@]}" "-D$1" "$here/probe_g.cpp" >"$ob/probe-$1.log" 2>&1
}
if probe PROBE_G_CONTROL; then
    ok "PROBE-G control compiles"
else
    bad "PROBE-G control does not compile: $(head -n 3 "$ob/probe-PROBE_G_CONTROL.log" | tr '\n' ' ')"
fi
for p in PROBE_G_COPY PROBE_G_MOVE; do
    if probe "$p"; then
        bad "$p compiled: the guard must not be copyable or movable"
    elif grep -qi 'delete' "$ob/probe-$p.log"; then
        ok "$p fails to compile (deleted)"
    else
        bad "$p failed for another reason: $(head -n 3 "$ob/probe-$p.log" | tr '\n' ' ')"
    fi
done

echo "== ps3dk-prof runtime host tests: $pass passed, $fail failed, $skip skipped, $notrun registry tests not run"
{ echo "pass $pass"; echo "fail $fail"; echo "skip $skip"; echo "not_run $notrun"; } >"$out/counts.txt"
[ "$fail" -eq 0 ]

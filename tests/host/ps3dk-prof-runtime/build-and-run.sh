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
# sanitizer report.  A failed control fails the variant and its 103 rows are NOT RUN (detection not proven).
# Plain and asan launches are unchanged.
# The TSan launch path (rrun, xrun, the setarch identity check, the sanitizer probes, the detection controls and the
# variant admission) lives in driver-tsan.sh, sourced below.  It does not rely on errexit: every launch and evidence
# record is checked, a pre-launch record failure prevents the launch, and any record failure is a FAIL (a recorder
# failure, counted separately), never a silent success.  driver-tsan-selftest.sh exercises it with fake children.
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
# Only the portable public API belongs on this host compiler's include path.
# The SDK's libc/pthread wrappers target newlib and must not wrap host glibc.
public_inc="$out/public-include"
mkdir -p "$public_inc/ps3dk"
cp -- "$root/sdk/include/ps3dk/prof.h" "$public_inc/ps3dk/prof.h"
source_hash="$(sha256sum "$root/sdk/include/ps3dk/prof.h")"
copy_hash="$(sha256sum "$public_inc/ps3dk/prof.h")"
if [ "${source_hash%% *}" != "${copy_hash%% *}" ]; then
    echo "FAIL public prof.h copy differs from source" >&2
    exit 1
fi
inc=(-I"$public_inc" -I"$root/sdk/libps3dkprof/src/core" -I"$root/sdk/libps3dkprof/src/pal")
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

# The TSan launch path's own self-test (driver-tsan-selftest.sh, fake children only: no compiler, no TSan) runs first,
# so a recorder that can lose a failure is caught before anything is built.
dts=0
bash "$here/driver-tsan-selftest.sh" "$out/driver-tsan-selftest" >"$out/driver-tsan-selftest.out" 2>&1 || dts=$?
if [ "$dts" = 0 ]; then
    ok "driver-tsan selftest (driver-tsan-selftest.out)"
else
    bad "driver-tsan selftest exit $dts (driver-tsan-selftest.out)"
fi

# clang warns about side effects inside sizeof (-Wunevaluated-expression); unevaluated side effects are the point here.
noeval() { local v; v="$("$1" --version 2>&1 || true)"; case "$v" in *[Cc]lang*) echo -Wno-unevaluated-expression ;; esac; }
cc_noeval="$(noeval "$CC")"
cxx_noeval="$(noeval "$CXX")"

if [ -n "${READER:-}" ]; then
    reader=("$READER" check)
    reporter=("$READER" report)
else
    echo "== building the Rust reader"
    cargo build -q --manifest-path "$root/tools/Cargo.toml" -p ps3dk-prof
    reader=(cargo run -q --manifest-path "$root/tools/Cargo.toml" -p ps3dk-prof -- check)
    reporter=(cargo run -q --manifest-path "$root/tools/Cargo.toml" -p ps3dk-prof -- report)
fi

# check_capture <path> <want> <label>: the reader's streams and exit go through rrun, so a record that cannot be
# written is a FAIL (never a verdict).
check_capture() {
    local r=0 txt="$1.check.txt"
    rrun "$txt" "$1.check.err" "$1.check.rc" "${reader[@]}" "$1" || r=$?
    if [ "$r" != 0 ]; then
        bad "$3: the reader verdict could not be recorded (status $r, $1.check.*)"
        return 1
    fi
    if [ "$rrc" = "$2" ]; then
        return 0
    fi
    bad "$3: reader exit $rrc, want $2: $(head -n 1 "$txt") $(head -n 1 "$1.check.err")"
    return 1
}

# check_report <path> <mode> <text> <label>: a test's "REPORT <path> <mode> <text>" line.  The normal
# `ps3dk-prof report` runs once per capture through rrun (streams and exit recorded, exit 0 or 1 required: a verdict,
# not MALFORMED or an I/O error); its lines, leading spaces removed, go to <path>.report.trim.  HAS / LACKS: the exact
# line is present / absent (grep -Fx); MATCH / NOMATCH: a line matching the whole ERE is present / absent (grep -Ex).
# grep status 0 found, 1 absent, >1 an inspection failure (FAIL, never read as absent).
check_report() {
    local cap="$1" mode="$2" want="$3" r=0 rrcv flag
    if [ ! -f "$cap.report.rc" ]; then
        rrun "$cap.report.txt" "$cap.report.err" "$cap.report.rc" "${reporter[@]}" "$cap" || r=$?
        if [ "$r" != 0 ]; then
            bad "$4: the report could not be recorded (status $r, $cap.report.*)"
            return 1
        fi
        if ! sed 's/^[[:space:]]*//' "$cap.report.txt" >"$cap.report.trim"; then
            bad "$4: the report could not be trimmed ($cap.report.trim)"
            return 1
        fi
    fi
    if ! rrcv="$(cat "$cap.report.rc")" || [ ! -f "$cap.report.trim" ]; then
        bad "$4: the report record is incomplete ($cap.report.*)"
        return 1
    fi
    if [ "$rrcv" != 0 ] && [ "$rrcv" != 1 ]; then
        bad "$4: report exit $rrcv: $(head -n 1 "$cap.report.err")"
        return 1
    fi
    case "$mode" in
    HAS | LACKS) flag=-Fxq ;;
    MATCH | NOMATCH) flag=-Exq ;;
    *)
        bad "$4: unknown REPORT mode '$mode'"
        return 1
        ;;
    esac
    if [ -z "$want" ]; then
        bad "$4: REPORT $mode with no text"
        return 1
    fi
    if ! grep_status "$flag" -e "$want" "$cap.report.trim"; then
        bad "$4: report inspection failed (REPORT $mode $want)"
        return 1
    fi
    case "$mode:$gst" in
    HAS:0 | MATCH:0 | LACKS:1 | NOMATCH:1) return 0 ;;
    esac
    bad "$4: report $mode failed: '$want' ($cap.report.txt)"
    return 1
}

# ---- 1. build ---------------------------------------------------------------------------------------------------------

declare -A vflags=([plain]="" [asan]="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" [tsan]="-fsanitize=thread")
variants=()

build_variant() {
    local v="$1"
    local dir="$out/build-$v" s o   # a separate local: in one `local`, $v would expand before v is set
    local extra
    # Called as `build_variant "$v" || rc=$?`, so errexit is off in here: every step is checked explicitly.
    if ! read -r -a extra <<<"${vflags[$v]}"; then
        recorder "build $v: cannot read the variant flags"
        return 1
    fi
    local cflags=(-std=c11 -O1 -g -pthread -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS "${inc[@]}" "${warn[@]}" "${extra[@]}")
    local objs=()
    mkd "$dir" || return 1
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

# shellcheck source=driver-tsan.sh
. "$here/driver-tsan.sh"

# Why each variant's tests did not run, when they did not ("" = built).
declare -A notbuilt=()
for v in plain asan tsan; do
    if ! admit_variant "$v"; then
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
# TSAN_OPTIONS, with the declared exitcode, was exported by driver-tsan.sh before the sanitizer probes ran.

# controls.tsv: the TSan detection controls (separate from results.tsv): control, result, reason.
controls="$out/controls.tsv"
printf 'control\tresult\treason\n' >"$controls"

# The frozen registry: the 103 test names, sorted, unique.  Each built variant's `rt --list` must equal it exactly.
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
        if ! admit_tsan_controls; then
            not_run_all "$v" "TSan detection not proven: a control failed or could not be recorded (controls.tsv)"
            continue
        fi
    fi
    bin="$out/build-$v/rt"
    mkdir -p "$out/run-$v"
    lst="$out/run-$v/list.txt"
    rc=0
    if [ "$v" = tsan ]; then
        xr=0
        xrun "$out/run-$v/list" "$lst" "$out/run-$v/list.stderr" "$out/run-$v/list.rc" 60 "$bin" --list || xr=$?
        rc=$xrc
        if [ "$xr" != 0 ]; then
            bad "$v: rt --list launch could not be recorded (xrun status $xr, $out/run-$v/)"
            not_run_all "$v" "rt --list launch record failed"
            continue
        fi
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
            xr=0
            xrun "$d/exec" "$d/stdout" "$d/stderr" "$d/rc" 180 "$bin" "$t" "$d" || xr=$?
            rc=$xrc
            if [ "$xr" != 0 ]; then
                bad "$v $t: launch could not be recorded (xrun status $xr, $d/)"
                result "$v" "$t" FAIL "launch record failed"
                continue
            fi
        else
            timeout 180 "$bin" "$t" "$d" >"$d/stdout" 2>"$d/stderr" || rc=$?
            echo "$rc" >"$d/rc"
        fi
        if [ "$rc" -ne 0 ]; then
            bad "$v $t: exit $rc ($d/stderr): $(grep -m 1 -E 'CHECK|ERROR|WARNING|runtime error' "$d/stderr" || true)"
            result "$v" "$t" FAIL "exit $rc"
            continue
        fi
        # grep status 0 = a report, 1 = none, >1 = the stderr record could not be inspected (FAIL, never "none").
        if ! grep_status -qE 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' \
            "$d/stderr"; then
            bad "$v $t: stderr could not be inspected for sanitizer reports ($d/stderr)"
            result "$v" "$t" FAIL "stderr inspection failed"
            continue
        fi
        if [ "$gst" = 0 ]; then
            bad "$v $t: sanitizer report ($d/stderr)"
            result "$v" "$t" FAIL "sanitizer report"
            continue
        fi
        caps_ok=1
        while read -r tag path tag2 want; do
            case "$tag" in
            CAPTURE)
                [ "$tag2" = EXPECT ] || continue
                check_capture "$path" "$want" "$v $t" || caps_ok=0
                ;;
            REPORT) check_report "$path" "$tag2" "$want" "$v $t" || caps_ok=0 ;;
            esac
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
    # Symbol lists through files and grep_status (0 match, 1 none, >1 FAIL), never `grep ... || true`.
    awk 'NF >= 3 {print $3}' "$ob/nm-defined.txt" | sort -u >"$ob/nm-defined-names.txt" || bad "defined-symbol list failed"
    grep_status -E '^ps3dk_prof_[a-z]' "$ob/nm-defined-names.txt" >"$ob/nm-public.txt" ||
        bad "public-symbol inspection failed"
    grep_status -vE '^ps3dk_prof_' "$ob/nm-defined-names.txt" >"$ob/nm-other.txt" || bad "other-symbol inspection failed"
    public="$(tr '\n' ' ' <"$ob/nm-public.txt" | sed 's/ $//')"
    other="$(cat "$ob/nm-other.txt")"
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
    if ! grep_status -q 'ps3dk_prof_test_' "$ob/nm-all.txt"; then
        bad "archive symbol list could not be inspected for test-hook symbols"
    elif [ "$gst" = 0 ]; then
        bad "archive objects contain a ps3dk_prof_test_ symbol"
    else
        ok "archive objects contain no test-hook symbol"
    fi
    # The SDK form of the source itself: preprocessed without PS3DK_PROF_TEST_HOOKS, prof_core.c names no test hook
    # (the ID_COLLISION candidate-id hook included).  File-based grep, not a pipe.
    if "$CC" -std=c11 -E -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST "${inc[@]}" "$(src_of prof_core)" \
        >"$ob/prof_core-nohooks.i" 2>"$ob/prof_core-nohooks.log"; then
        # grep status 0 = a forbidden hook name FOUND (FAIL), 1 = absent (PASS), >1 = inspection failure (FAIL).
        if ! grep_status -q -e 'candidate_desc_id' -e 'ps3dk_prof_test_hooks' "$ob/prof_core-nohooks.i"; then
            bad "hook-free preprocessed prof_core.c could not be inspected"
        elif [ "$gst" = 0 ]; then
            bad "hook-free preprocessed prof_core.c names a test hook"
        else
            ok "hook-free preprocessed prof_core.c names no test hook"
        fi
    else
        bad "hook-free preprocess of prof_core.c failed"
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
        elif ! grep_status -q 'ps3dk_prof' "$o.nm-u.txt"; then
            bad "$tag: OB-N: the undefined-symbol list could not be inspected"
        elif [ "$gst" = 0 ]; then
            bad "$tag: OB-N: the disabled object references $(grep -m 1 ps3dk_prof "$o.nm-u.txt" || true)"
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

echo "== ps3dk-prof runtime host tests: $pass passed, $fail failed, $skip skipped, $notrun registry tests not run, $recfail recorder failures"
{ echo "pass $pass"; echo "fail $fail"; echo "skip $skip"; echo "not_run $notrun"; echo "recorder_failures $recfail"; } >"$out/counts.txt"
[ "$fail" -eq 0 ] && [ "$recfail" -eq 0 ]

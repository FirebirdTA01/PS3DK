#!/usr/bin/env bash
# Host build and run of the ps3dk-prof runtime B tests (step 3, host half).
#
#   tests/host/ps3dk-prof-runtime/build-and-run.sh <out-dir>
#
# With the HOST compiler only (CC, CXX; default cc and c++), into <out-dir>:
#   1. builds the portable core + the host PAL + the tests in three variants: plain, asan (ASan + UBSan) and tsan.
#      A sanitizer the compiler cannot link is SKIPped; REQUIRE_SANITIZERS=1 makes that a failure;
#   2. runs every rt_* test (`rt --list`) in its own process with a fresh output directory;
#   3. checks every capture a test names ("CAPTURE <path> EXPECT <n>") with the Rust reader,
#      `cargo run -p ps3dk-prof -- check <path>` (READER=<binary> uses a prebuilt reader instead), and compares the exit;
#   4. runs the host off-build checks: OB-E1 (C) and OB-E2 (C++) built enabled, undefined and =0 and RUN; host OB-P
#      (preprocessed disabled source); OB-N (no profiler symbol referenced by the disabled objects); PROBE-G (the C++
#      guard is not copyable or movable); and the archive-content check (the hook-free objects export exactly the API
#      under ps3dk_prof_, internals only under ps3dk_prof__, no ps3dk_prof_test_ symbol, no release function).
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
noeval() { if "$1" --version 2>/dev/null | grep -qi clang; then echo -Wno-unevaluated-expression; fi; }
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

sanitizer_links() {
    local v="$1" dir="$out/probe-$v" extra
    read -r -a extra <<<"${vflags[$v]}"
    mkdir -p "$dir"
    printf 'int main(void){return 0;}\n' >"$dir/p.c"
    "$CC" "${extra[@]}" "$dir/p.c" -o "$dir/p" >/dev/null 2>&1 && "$dir/p" >/dev/null 2>&1
}

for v in plain asan tsan; do
    if [ "$v" != plain ] && ! sanitizer_links "$v"; then
        if [ "${REQUIRE_SANITIZERS:-0}" = 1 ]; then
            bad "build $v: the compiler cannot build and run $v programs"
        else
            skp "build $v: the compiler cannot build and run $v programs"
        fi
        continue
    fi
    echo "== building $v"
    if build_variant "$v"; then
        ok "build $v"
        variants+=("$v")
    else
        bad "build $v"
    fi
done

# ---- 2 + 3. run every test and check its captures --------------------------------------------------------------------

export ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=1"

for v in "${variants[@]}"; do
    bin="$out/build-$v/rt"
    for t in $("$bin" --list); do
        d="$out/run-$v/$t"
        rm -rf "$d"
        mkdir -p "$d"
        rc=0
        timeout 180 "$bin" "$t" "$d" >"$d/stdout" 2>"$d/stderr" || rc=$?
        if [ "$rc" -ne 0 ]; then
            bad "$v $t: exit $rc ($d/stderr): $(grep -m 1 -E 'CHECK|ERROR|WARNING|runtime error' "$d/stderr" || true)"
            continue
        fi
        if grep -qE 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' "$d/stderr"; then
            bad "$v $t: sanitizer report ($d/stderr)"
            continue
        fi
        caps_ok=1
        while read -r tag path tag2 want; do
            [ "$tag" = CAPTURE ] && [ "$tag2" = EXPECT ] || continue
            check_capture "$path" "$want" "$v $t" || caps_ok=0
        done <"$d/stdout"
        [ "$caps_ok" = 1 ] && ok "$v $t"
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
    defined="$(nm -g --defined-only "${libobjs[@]}" | awk 'NF >= 3 {print $3}' | sort -u)"
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
    if nm "${libobjs[@]}" | grep -q 'ps3dk_prof_test_'; then
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
        if nm -u "$o.o" | grep -q 'ps3dk_prof'; then
            bad "$tag: OB-N: the disabled object references $(nm -u "$o.o" | grep ps3dk_prof | head -n 1)"
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

echo "== ps3dk-prof runtime host tests: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]

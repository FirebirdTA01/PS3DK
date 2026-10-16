#!/usr/bin/env bash
# The lv2 POSIX declarations (newlib 0018: pthread.h, time.h, sched.h; plus sysconf and the Issue 8 clock waits)
# have C linkage from C++.  A declaration that lands outside its header's extern "C" block compiles, and fails
# only at link time with a C++-mangled undefined reference (clock_gettime(unsigned long, timespec*) in
# libstdc++'s chrono.cc, 2026-10-15).
#
#  0. self-test: a failing compile is reported by its status; a planted C++-linkage declaration is reported;
#  1. per header (time.h, sched.h, unistd.h, pthread.h), a C++ unit that includes only that header takes the
#     address of each function it declares, at -std=c++17 and -std=gnu++20, ILP32 and LP64: each must be an
#     undefined reference by its plain C name, with no mangled (_Z) reference to it.
#
# usage: lv2-decls-cxx-linkage-test.sh [--ppu-gxx <powerpc64-ps3-elf-g++>]
#   With no argument: $PS3DEV's g++; it skips without PS3DEV or when newlib does not carry 0018 (no
#   _LV2_TIME_DECLS).  A named compiler never skips.  Exit 0 pass (or skip), 1 fail, 2 usage or missing input.
set -u
gxx=""
while [ $# -gt 0 ]; do
    case $1 in
        --ppu-gxx) gxx=$2; shift 2 ;;
        *) echo "usage: $0 [--ppu-gxx <g++>]"; exit 2 ;;
    esac
done
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
probe() { printf '#include <time.h>\n#ifndef _LV2_TIME_DECLS\n#error no lv2 declarations\n#endif\n' > "$work/p.cpp"
          "$gxx" -fsyntax-only "$work/p.cpp" > "$work/p.log" 2>&1; }
if [ -z "$gxx" ]; then
    gxx="${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-g++"
    if [ -z "${PS3DEV:-}" ] || [ ! -x "$gxx" ]; then echo "SKIP: lv2-decls-cxx-linkage-test (no PS3DEV compiler)"; exit 0; fi
    if ! probe; then
        if grep -q 'no lv2 declarations' "$work/p.log"; then
            echo "SKIP: lv2-decls-cxx-linkage-test (newlib without the lv2 declarations)"; exit 0
        fi
        echo "lv2-decls-cxx-linkage: FAIL the probe compile failed"; head -5 "$work/p.log" | sed 's/^/    /'
        echo "FAIL: lv2-decls-cxx-linkage-test"; exit 1
    fi
fi
[ -x "$gxx" ] || { echo "lv2-decls-cxx-linkage: no compiler at '$gxx'"; exit 2; }
nm="${gxx%g++}nm"
[ -x "$nm" ] || { echo "lv2-decls-cxx-linkage: no nm at '$nm'"; exit 2; }

status=0
ok()   { echo "lv2-decls-cxx-linkage: ok   $*"; }
fail() { echo "lv2-decls-cxx-linkage: FAIL $*"; status=1; }
export PS3DK_NO_DEFAULT_INCLUDE=1
inc=(); [ -n "${PS3DK:-}" ] && inc=(-isystem "$PS3DK/ppu/include")

# One group per header: each unit includes ONLY its header, first, so linkage cannot come from another header's
# extern "C" block (pthread.h includes time.h inside its own block, which hid the round-9 time.h fault).
groups="time.h:clock_gettime,nanosleep
sched.h:sched_yield,sched_get_priority_min,sched_get_priority_max
unistd.h:sysconf
pthread.h:pthread_create,pthread_join,pthread_detach,pthread_self,pthread_equal,pthread_exit,pthread_once,pthread_key_create,pthread_key_delete,pthread_getspecific,pthread_setspecific,pthread_mutex_init,pthread_mutex_destroy,pthread_mutex_lock,pthread_mutex_trylock,pthread_mutex_unlock,pthread_mutex_timedlock,pthread_mutex_clocklock,pthread_mutexattr_init,pthread_mutexattr_destroy,pthread_mutexattr_settype,pthread_mutexattr_gettype,pthread_cond_init,pthread_cond_destroy,pthread_cond_wait,pthread_cond_timedwait,pthread_cond_clockwait,pthread_cond_signal,pthread_cond_broadcast,pthread_rwlock_init,pthread_rwlock_destroy,pthread_rwlock_rdlock,pthread_rwlock_wrlock,pthread_rwlock_unlock,pthread_rwlock_timedrdlock,pthread_rwlock_timedwrlock,pthread_rwlock_clockrdlock,pthread_rwlock_clockwrlock,pthread_attr_init,pthread_attr_destroy,pthread_attr_setstacksize,pthread_attr_setdetachstate,pthread_attr_setschedpolicy,pthread_attr_setinheritsched"

# unit <file> <header> <names> [planted]: a C++ unit that includes HEADER alone and takes every name's address
# (and, planted, the address of a function declared with C++ linkage).
unit() {
    { echo '#define _GNU_SOURCE 1'; echo "#include <$2>"
      [ "${4:-}" = planted ] && echo 'int lv2_planted_cxx_decl(int);'
      echo 'void *lv2_addrs[] = {'
      for n in $3; do echo "  (void *)&$n,"; done
      [ "${4:-}" = planted ] && echo '  (void *)&lv2_planted_cxx_decl,'
      echo '};'; } > "$1"
}
# check <object> <label> <names>: every name is a plain undefined reference, none mangled.
check() {
    local o=$1 label=$2 bad=0
    "$nm" -u "$o" > "$work/u.txt" 2>"$work/nm.err" || { fail "$label: nm failed"; return 1; }
    for n in $3; do
        if ! grep -qx "[[:space:]]*U $n" "$work/u.txt"; then fail "$label: no plain C reference to $n"; bad=1; fi
        # A mangled reference to a global function N reads _Z<length of N><N><parameter types>.
        if grep -q "U _Z${#n}${n}" "$work/u.txt"; then
            fail "$label: $n is referenced C++-mangled: $(grep "U _Z${#n}${n}" "$work/u.txt" | head -1)"; bad=1
        fi
    done
    return $bad
}

# 0
unit "$work/a.cpp" time.h "clock_gettime nanosleep"
if "$gxx" -std=c++17 -fno-such-flag-linkage-selftest "${inc[@]}" -c "$work/a.cpp" -o "$work/bad.o" > /dev/null 2>&1; then
    fail "self-test: an invalid flag compiled (the compiler status cannot be trusted)"
else ok "self-test: a failing compile is reported by its status"; fi
unit "$work/planted.cpp" time.h "clock_gettime" planted
if "$gxx" -std=c++17 "${inc[@]}" -c "$work/planted.cpp" -o "$work/planted.o" > "$work/cc.log" 2>&1; then
    out=$(status=0; check "$work/planted.o" planted "clock_gettime lv2_planted_cxx_decl"; echo "rc=$?")
    if grep -q "lv2_planted_cxx_decl is referenced C++-mangled" <<< "$out"; then
        ok "self-test: a planted C++-linkage declaration is reported"
    else
        fail "self-test: a planted C++-linkage declaration was not reported"
    fi
else
    fail "self-test: the planted unit did not compile"; head -5 "$work/cc.log" | sed 's/^/    /'
fi

# 1
total=0
while IFS=: read -r header list; do
    names=$(printf '%s' "$list" | tr ',' ' ')
    total=$((total + $(echo $names | wc -w)))
    unit "$work/u.cpp" "$header" "$names"
    for abi in "" -mlp64; do
        for std in -std=c++17 -std=gnu++20; do
            if [ -n "$abi" ]; then label="<$header> LP64 $std"; else label="<$header> ILP32 $std"; fi
            if ! "$gxx" $abi $std "${inc[@]}" -c "$work/u.cpp" -o "$work/u.o" > "$work/cc.log" 2>&1; then
                fail "$label: compile failed"; head -5 "$work/cc.log" | sed 's/^/    /'; continue
            fi
            check "$work/u.o" "$label" "$names" && ok "$label: $(echo $names | wc -w) functions, all C linkage"
        done
    done
done <<< "$groups"
[ "$total" -eq 50 ] || fail "the groups list $total functions, expected 50"

[ $status -eq 0 ] && echo "PASS: lv2-decls-cxx-linkage-test" || echo "FAIL: lv2-decls-cxx-linkage-test"
exit $status

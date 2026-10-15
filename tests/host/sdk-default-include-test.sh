#!/usr/bin/env bash
# The PPU driver puts the SDK include directory on the default path
# (patches/ppu/gcc-16.x/0068, t_d2b6fe51).  A C file that includes only
# <pthread.h>, <time.h> and <sched.h> must compile with implicit
# declarations as errors, as GCC 14+ makes them:
#
#  1. PS3DK unset: the SDK installed with the compiler, ILP32 and LP64;
#  2. PS3DK set: that SDK, and from a directory whose path has a space;
#  3. PS3DK naming a directory without SDK headers: no fallback to the
#     installed SDK (the control compile fails);
#  4. PS3DK_NO_DEFAULT_INCLUDE=1 drops it (fails), =0 keeps it;
#  5. -nostdinc drops it (the search list does not name it);
#  6. an explicit -isystem of the same directory leaves it in the search
#     list once.
#
# usage: sdk-default-include-test.sh [--ppu-gcc <powerpc64-ps3-elf-gcc> --sdk <PS3DK root>]
#   <PS3DK root> holds ppu/include (the packaged $PS3DK, or staged $PS3DEV/ps3dk).
#   With no arguments: $PS3DEV's compiler and $PS3DEV/ps3dk; it skips without
#   PS3DEV or when that compiler has no %:ps3dk-include() (GCC 12.4).  Named
#   inputs never skip.
# Exit 0 pass (or skip), 1 fail, 2 usage or missing named inputs.
set -u
gcc=""; sdk=""
while [ $# -gt 0 ]; do
    case $1 in
        --ppu-gcc) gcc=$2; shift 2 ;;
        --sdk) sdk=$2; shift 2 ;;
        *) echo "usage: $0 [--ppu-gcc <gcc> --sdk <PS3DK root>]"; exit 2 ;;
    esac
done
if [ -z "$gcc" ] && [ -z "$sdk" ]; then
    gcc="${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc"; sdk="${PS3DEV:-}/ps3dk"
    if [ -z "${PS3DEV:-}" ] || [ ! -x "$gcc" ]; then
        echo "SKIP: sdk-default-include-test (no PS3DEV compiler)"; exit 0
    fi
    if ! "$gcc" -dumpspecs | grep -q 'ps3dk-include'; then
        echo "SKIP: sdk-default-include-test ($gcc has no default SDK include directory)"; exit 0
    fi
fi
[ -x "$gcc" ] || { echo "sdk-default-include: no compiler at '$gcc'"; exit 2; }
[ -f "$sdk/ppu/include/sys/lv2_syscall.h" ] || { echo "sdk-default-include: no SDK headers under '$sdk/ppu/include'"; exit 2; }
inc=$(cd "$sdk/ppu/include" && pwd -P)

status=0
ok()   { echo "sdk-default-include: ok   $*"; }
fail() { echo "sdk-default-include: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

cat > "$work/t.c" <<'EOF'
#include <pthread.h>
#include <sched.h>
#include <time.h>
static void *run(void *a) { return a; }
int f(void)
{
    pthread_t t;
    struct timespec ts = { 0, 1000 };
    if (pthread_create(&t, 0, run, 0) != 0)
        return -1;
    nanosleep(&ts, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return sched_get_priority_min(SCHED_OTHER) + sched_get_priority_max(SCHED_OTHER);
}
EOF
flags="-std=c11 -fsyntax-only -Werror=implicit-function-declaration"

# compiles <label> <expect pass|fail> [env assignments...] -- [extra flags...]
compiles() {
    local label=$1 want=$2; shift 2
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    if env -u PS3DK -u PS3DK_NO_DEFAULT_INCLUDE "${envs[@]}" "$gcc" $flags "$@" "$work/t.c" > "$work/out" 2>&1; then
        got=pass
    else
        got=fail
    fi
    if [ $got = "$want" ]; then
        ok "$label: compile ${got}ed as expected"
    else
        fail "$label: compile ${got}ed, expected $want"; head -5 "$work/out" | sed 's/^/    /'
    fi
}

# times_listed <env...> -- <flags...>: how often the SDK directory is in the <...> search list.
times_listed() {
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    env -u PS3DK -u PS3DK_NO_DEFAULT_INCLUDE "${envs[@]}" "$gcc" -E -v -x c "$@" /dev/null -o /dev/null 2>&1 |
        awk '/search starts here:/{on=1; next} /^End of search list/{on=0} on {sub(/^ +/, ""); print}' |
        while read -r d; do [ -d "$d" ] && (cd "$d" && pwd -P); done | grep -cxF "$inc"
}

# 1. installed SDK, both ABIs
compiles "PS3DK unset, ILP32" pass --
compiles "PS3DK unset, LP64" pass -- -mlp64
n=$(times_listed --); [ "$n" = 1 ] && ok "PS3DK unset: SDK directory listed once" || fail "PS3DK unset: SDK directory listed $n times"

# 2. the selected SDK, also through a path with a space
compiles "PS3DK set" pass PS3DK="$sdk" --
mkdir -p "$work/sdk with space/ppu"
ln -s "$inc" "$work/sdk with space/ppu/include"
compiles "PS3DK path with a space" pass PS3DK="$work/sdk with space" --
n=$(times_listed PS3DK="$work/sdk with space" --); [ "$n" = 1 ] && ok "PS3DK with a space: SDK directory listed once" || fail "PS3DK with a space: listed $n times"

# 3. a PS3DK without headers selects nothing
mkdir -p "$work/empty/ppu/include"
compiles "PS3DK without SDK headers (control)" fail PS3DK="$work/empty" --

# 4. the opt-out
compiles "PS3DK_NO_DEFAULT_INCLUDE=1 (control)" fail PS3DK_NO_DEFAULT_INCLUDE=1 --
compiles "PS3DK_NO_DEFAULT_INCLUDE=0" pass PS3DK_NO_DEFAULT_INCLUDE=0 --

# 5. -nostdinc
n=$(times_listed -- -nostdinc); [ "$n" = 0 ] && ok "-nostdinc: SDK directory not listed" || fail "-nostdinc: SDK directory listed $n times"

# 6. an explicit -isystem of the same directory
n=$(times_listed -- -isystem "$inc"); [ "$n" = 1 ] && ok "explicit -isystem: SDK directory listed once" || fail "explicit -isystem: listed $n times"

[ $status -eq 0 ] && echo "PASS: sdk-default-include-test"
exit $status

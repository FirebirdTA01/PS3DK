#!/usr/bin/env bash
# The PPU driver puts the SDK include directory on the default path
# (patches/ppu/gcc-16.x/0068, t_d2b6fe51).  A C file that includes
# <pthread.h>, <time.h>, <sched.h> and the SDK-only <sys/ppu_thread.h> must
# compile with implicit declarations as errors, as GCC 14+ makes them.
# <sys/ppu_thread.h> is what makes the controls below fail without the SDK
# directory: since newlib 0018 (the GCC 16 thread layer) newlib declares the
# lv2 pthread/time/sched functions itself, so those three alone resolve
# without it.
#
#  1. PS3DK unset: the SDK installed with the compiler, ILP32 and LP64;
#  2. PS3DK set: that SDK, and from directories whose paths hold a space,
#     "%s" and "% " (the driver parses the directory as spec text);
#  3. PS3DK naming a directory without SDK headers, or set to the empty
#     string: no fallback to the installed SDK (the control compile fails);
#  4. PS3DK_NO_DEFAULT_INCLUDE=1 drops it (fails), =0 keeps it;
#  5. -nostdinc drops it (the search list does not name it);
#  6. an explicit -isystem of the same directory leaves it in the search
#     list once.
#
# Run against an SDK that has <sys/ppu_thread.h> and, for a newlib without
# 0018, the sched.h wrapper (qwen/t_a2644ed1-sched, 9d622bb0).
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
#include <sys/ppu_thread.h>
static void *run(void *a) { return a; }
int f(void)
{
    pthread_t t;
    sys_ppu_thread_t self;
    struct timespec ts = { 0, 1000 };
    if (pthread_create(&t, 0, run, 0) != 0)
        return -1;
    nanosleep(&ts, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sys_ppu_thread_get_id(&self);
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

# times_listed <want dir> <env...> -- <flags...>: how often <want dir> is in the <...> search list.  Directories are
# compared by identity (-ef), not by spelling: a Windows-hosted driver prints C:/... paths that Git Bash spells
# /tmp/... or /c/..., and Git Bash's ln -s makes a copy rather than a link.
# Every step is status-checked, none in a pipeline: the -E -v run must succeed, its output must hold both search-list
# markers, and the parse into a file must succeed.  Any infrastructure failure prints "x", a non-count, so it fails an
# expected 1 and an expected 0 alike.
times_listed() {
    local want=$1 envs=() d n=0; shift
    while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    if ! env -u PS3DK -u PS3DK_NO_DEFAULT_INCLUDE "${envs[@]}" "$gcc" -E -v -x c "$@" /dev/null -o /dev/null > "$work/v.txt" 2>&1; then
        echo x; return
    fi
    if ! grep -q 'search starts here:' "$work/v.txt" || ! grep -q '^End of search list' "$work/v.txt"; then
        echo x; return
    fi
    if ! awk '/search starts here:/{on=1; next} /^End of search list/{on=0} on {sub(/^ +/, ""); print}' "$work/v.txt" > "$work/dirs.txt"; then
        echo x; return
    fi
    while IFS= read -r d; do
        if [ -d "$d" ] && [ "$d" -ef "$want" ]; then n=$((n + 1)); fi
    done < "$work/dirs.txt"
    echo "$n"
}

# Controls for times_listed itself: a failing -E -v run, and a parser that prints its real output and then fails,
# must each give the non-count "x" (so neither an expected 1 nor an expected 0 can pass on them).
real_gcc=$gcc
mkdir -p "$work/stub" "$work/awkfail"
printf '#!/bin/sh\necho "stand-in: -E -v fails" >&2\nexit 1\n' > "$work/stub/gcc"; chmod +x "$work/stub/gcc"
gcc="$work/stub/gcc"; n=$(times_listed "$inc" --); gcc=$real_gcc
[ "$n" = x ] && ok "self-test: a failing -E -v run is not a count" || fail "self-test: a failing -E -v run counted as '$n'"
real_awk=$(command -v awk)
printf '#!/bin/sh\n"%s" "$@"\nexit 1\n' "$real_awk" > "$work/awkfail/awk"; chmod +x "$work/awkfail/awk"
n=$(PATH="$work/awkfail:$PATH" times_listed "$inc" --)
[ "$n" = x ] && ok "self-test: a parser that prints and then fails is not a count" || fail "self-test: a failing parser counted as '$n'"
n=$(times_listed "$inc" --)
[ "$n" = 1 ] && ok "self-test: the real parser counts the installed SDK once" || fail "self-test: the real parser counted '$n'"

# 1. installed SDK, both ABIs
compiles "PS3DK unset, ILP32" pass --
compiles "PS3DK unset, LP64" pass -- -mlp64
n=$(times_listed "$inc" --); [ "$n" = 1 ] && ok "PS3DK unset: SDK directory listed once" || fail "PS3DK unset: SDK directory listed $n times"

# 2. the selected SDK, also through a path with a space
compiles "PS3DK set" pass PS3DK="$sdk" --
mkdir -p "$work/sdk with space/ppu"
ln -s "$inc" "$work/sdk with space/ppu/include"
compiles "PS3DK path with a space" pass PS3DK="$work/sdk with space" --
n=$(times_listed "$work/sdk with space/ppu/include" PS3DK="$work/sdk with space" --); [ "$n" = 1 ] && ok "PS3DK with a space: SDK directory listed once" || fail "PS3DK with a space: listed $n times"
for name in "sdk %s" "sdk % x"; do
    mkdir -p "$work/$name/ppu"
    ln -s "$inc" "$work/$name/ppu/include"
    compiles "PS3DK path with '$name'" pass PS3DK="$work/$name" --
    n=$(times_listed "$work/$name/ppu/include" PS3DK="$work/$name" --); [ "$n" = 1 ] && ok "PS3DK '$name': SDK directory listed once" || fail "PS3DK '$name': listed $n times"
done

# 3. a PS3DK without headers selects nothing
mkdir -p "$work/empty/ppu/include"
compiles "PS3DK without SDK headers (control)" fail PS3DK="$work/empty" --
compiles "PS3DK set to the empty string (control)" fail PS3DK= --

# 4. the opt-out
compiles "PS3DK_NO_DEFAULT_INCLUDE=1 (control)" fail PS3DK_NO_DEFAULT_INCLUDE=1 --
compiles "PS3DK_NO_DEFAULT_INCLUDE=0" pass PS3DK_NO_DEFAULT_INCLUDE=0 --

# 5. -nostdinc
n=$(times_listed "$inc" -- -nostdinc); [ "$n" = 0 ] && ok "-nostdinc: SDK directory not listed" || fail "-nostdinc: SDK directory listed $n times"

# 6. an explicit -isystem of the same directory
n=$(times_listed "$inc" -- -isystem "$inc"); [ "$n" = 1 ] && ok "explicit -isystem: SDK directory listed once" || fail "explicit -isystem: listed $n times"

[ $status -eq 0 ] && echo "PASS: sdk-default-include-test"
exit $status

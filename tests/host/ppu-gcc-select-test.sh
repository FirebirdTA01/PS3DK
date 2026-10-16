#!/usr/bin/env bash
# build-ppu-toolchain.sh --gcc=12|16 (stage 2 of the GCC transition), checked
# through --print-config without building anything:
#
#  1. the default is GCC 12.4.0 with gcc-12.x, gnu++11 host C++, build dir ppu;
#  2. --gcc=16 selects releases/gcc-16.1.0, gcc-16.x, no host -std pin and its own build
#     dir ppu-gcc16, with the same prefix rule ($PS3DEV/ppu);
#  3. --gcc=13 is refused, naming the supported values;
#  4. --gcc=16 --host=... is accepted over a matching native GCC 16 (build dir ppu-gcc16-<host>, no host -std pin),
#     refused without a native compiler; 12.4's --host keeps ppu-<host> and gnu++11.  The cross prefix's GCC major
#     comes from its install metadata (.ps3dk-gcc-version), fail closed: another major, missing, malformed or
#     unreadable metadata beside a gcc or gcc.exe is refused; matching metadata, or no compiler at all, is accepted;
#  5. a prefix holding GCC 12 refuses --gcc=16, and one holding GCC 16 refuses
#     the default, both exiting 1; a prefix holding the same major is accepted;
#  6. --clean with --print-config removes nothing (the clean happens after the
#     selection, so --gcc=16 --clean can never reach the 12.4 build dir).
#
# usage: ppu-gcc-select-test.sh
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script="$root/scripts/build-ppu-toolchain.sh"
status=0
fail() { echo "ppu-gcc-select: FAIL $*"; status=1; }
ok() { echo "ppu-gcc-select: ok   $*"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# run <ps3dev> <args...>: print-config in a clean environment; output in
# $work/out, exit status in $rc.
run() {
    local dev=$1; shift
    env -i HOME="$HOME" PATH="/usr/bin:/bin" PS3DEV="$dev" PS3_BUILD_ROOT="$work/build" \
        bash "$script" "$@" > "$work/out" 2>&1
    rc=$?
}
val() { sed -n "s/^$1=//p" "$work/out"; }

# expect <label> <key> <value>
expect() {
    local got; got=$(val "$2")
    [ "$got" = "$3" ] || { fail "$1: $2 is '$got', want '$3'"; sed 's/^/    /' "$work/out"; return 1; }
}

dev="$work/dev"
run "$dev" --print-config
if [ $rc -eq 0 ] && expect default gcc_tag releases/gcc-12.4.0 && expect default gcc_ver 12.4.0 \
   && expect default gcc_patches "$root/patches/ppu/gcc-12.x" && expect default host_cxx_std gnu++11 \
   && expect default prefix "$dev/ppu" && expect default build "$work/build/ppu"; then
    ok "default: GCC 12.4.0, gcc-12.x, gnu++11, build dir ppu"
else
    [ $rc -eq 0 ] || fail "default: exit $rc"
fi

run "$dev" --gcc=16 --print-config
if [ $rc -eq 0 ] && expect gcc16 gcc_tag releases/gcc-16.1.0 && expect gcc16 gcc_ver 16.1.0 \
   && expect gcc16 gcc_patches "$root/patches/ppu/gcc-16.x" && expect gcc16 host_cxx_std "" \
   && expect gcc16 prefix "$dev/ppu" && expect gcc16 build "$work/build/ppu-gcc16"; then
    ok "--gcc=16: GCC 16.1.0, gcc-16.x, no host -std pin, build dir ppu-gcc16"
else
    [ $rc -eq 0 ] || fail "--gcc=16: exit $rc"
fi
[ -f "$root/patches/ppu/gcc-16.x/series" ] || fail "patches/ppu/gcc-16.x/series is missing"

run "$dev" --gcc=13 --print-config
if [ $rc -eq 1 ] && grep -q "supported values are 12 and 16" "$work/out"; then
    ok "--gcc=13 is refused"
else
    fail "--gcc=13: exit $rc"; sed 's/^/    /' "$work/out"
fi

# A prefix whose gcc reports a given version.
fake() {
    mkdir -p "$1/ppu/bin"
    printf '#!/bin/sh\necho %s\n' "$2" > "$1/ppu/bin/powerpc64-ps3-elf-gcc"
    chmod +x "$1/ppu/bin/powerpc64-ps3-elf-gcc"
}
fake "$work/p12" 12.4.0
fake "$work/p16" 16.1.0

# 4. The Windows-hosted (--host) build: GCC 16 is accepted over a matching native GCC 16, in its own build dir
#    ppu-gcc16-<host>, while 12.4's cross build keeps ppu-<host>; without a native compiler it is still refused.
run "$dev" --gcc=16 --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 1 ] && grep -q "Native PPU toolchain not present" "$work/out"; then
    ok "--gcc=16 --host with no native compiler is refused"
else
    fail "--gcc=16 --host with no native compiler: exit $rc"; sed 's/^/    /' "$work/out"
fi
run "$work/p16" --gcc=16 --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 0 ] && expect cross16 build "$work/build/ppu-gcc16-x86_64-w64-mingw32" \
   && expect cross16 prefix "$work/p16.win/ppu" && expect cross16 gcc_ver 16.1.0 && expect cross16 host_cxx_std ""; then
    ok "--gcc=16 --host over a native GCC 16: accepted, build dir ppu-gcc16-x86_64-w64-mingw32, prefix .win/ppu"
else
    [ $rc -eq 0 ] || { fail "--gcc=16 --host over a native GCC 16: exit $rc"; sed 's/^/    /' "$work/out"; }
fi
run "$work/p12" --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 0 ] && expect cross12 build "$work/build/ppu-x86_64-w64-mingw32" && expect cross12 gcc_ver 12.4.0 \
   && expect cross12 host_cxx_std gnu++11; then
    ok "12.4 --host unchanged: build dir ppu-x86_64-w64-mingw32, gnu++11"
else
    [ $rc -eq 0 ] || { fail "12.4 --host: exit $rc"; sed 's/^/    /' "$work/out"; }
fi

# 4b. The cross prefix (<PS3DEV>.win/ppu) holds .exe files the build host cannot run: its GCC major comes from the
#     install metadata .ps3dk-gcc-version, fail closed.  win <dir> <gcc file name|-> <metadata|-|unreadable>
win() {
    local d=$1 name=$2 meta=$3
    rm -rf "$d.win"; mkdir -p "$d.win/ppu/bin"
    [ "$name" != - ] && printf 'not runnable here\n' > "$d.win/ppu/bin/$name"
    case $meta in
        -) ;;
        unreadable) echo 16.1.0 > "$d.win/ppu/.ps3dk-gcc-version"; chmod 000 "$d.win/ppu/.ps3dk-gcc-version" ;;
        *) printf '%s\n' "$meta" > "$d.win/ppu/.ps3dk-gcc-version" ;;
    esac
}
cross() { # <label> <gcc name> <metadata> <want rc> <want message|->
    win "$work/p16" "$2" "$3"
    run "$work/p16" --gcc=16 --host=x86_64-w64-mingw32 --print-config
    if [ $rc -eq "$4" ] && { [ "$5" = - ] || grep -q -- "$5" "$work/out"; }; then
        ok "cross prefix, $1: exit $rc"
    else
        fail "cross prefix, $1: exit $rc, want $4${5:+ with '$5'}"; sed 's/^/    /' "$work/out"
    fi
}
cross "gcc.exe with 12.4.0 metadata, --gcc=16"        powerpc64-ps3-elf-gcc.exe 12.4.0  1 "holds GCC 12.4.0 (per"
cross "extensionless gcc with 12.4.0 metadata"         powerpc64-ps3-elf-gcc     12.4.0  1 "holds GCC 12.4.0 (per"
cross "gcc.exe with 16.1.0 metadata (matching)"        powerpc64-ps3-elf-gcc.exe 16.1.0  0 -
cross "gcc.exe without metadata (unknown)"             powerpc64-ps3-elf-gcc.exe -       1 "unknown GCC version"
cross "gcc.exe with malformed metadata"                powerpc64-ps3-elf-gcc.exe banana  1 "not a GCC version"
cross "gcc.exe with empty metadata"                    powerpc64-ps3-elf-gcc.exe ""      1 "not a GCC version"
cross "no compiler, binutils only, no metadata"        powerpc64-ps3-elf-as.exe  -       0 -
win "$work/p16" powerpc64-ps3-elf-gcc.exe unreadable
if [ -r "$work/p16.win/ppu/.ps3dk-gcc-version" ]; then
    echo "ppu-gcc-select: skip unreadable metadata: chmod 000 still readable here (root, or Git Bash); runs on Linux CI"
else
    cross "gcc.exe with unreadable metadata"           powerpc64-ps3-elf-gcc.exe unreadable 1 "cannot be read"
fi
chmod 644 "$work/p16.win/ppu/.ps3dk-gcc-version" 2>/dev/null; rm -rf "$work/p16.win"
run "$work/p12" --gcc=16 --print-config
if [ $rc -eq 1 ] && grep -q "holds GCC 12.4.0; refusing to build GCC 16.1.0" "$work/out"; then
    ok "a GCC 12 prefix refuses --gcc=16"
else
    fail "GCC 12 prefix with --gcc=16: exit $rc"; sed 's/^/    /' "$work/out"
fi
run "$work/p16" --print-config
if [ $rc -eq 1 ] && grep -q "holds GCC 16.1.0; refusing to build GCC 12.4.0" "$work/out"; then
    ok "a GCC 16 prefix refuses the default 12.4"
else
    fail "GCC 16 prefix with the default: exit $rc"; sed 's/^/    /' "$work/out"
fi
run "$work/p16" --gcc=16 --print-config
[ $rc -eq 0 ] && ok "control: a GCC 16 prefix accepts --gcc=16" || { fail "GCC 16 prefix with --gcc=16: exit $rc"; sed 's/^/    /' "$work/out"; }
run "$work/p12" --print-config
[ $rc -eq 0 ] && ok "control: a GCC 12 prefix accepts the default" || { fail "GCC 12 prefix with the default: exit $rc"; sed 's/^/    /' "$work/out"; }

# A cross build copies the native prefix's target libraries: the native
# compiler must be the selected major too (the .win destination is empty).
run "$work/p16" --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 1 ] && grep -q "native prefix $work/p16/ppu holds GCC 16.1.0" "$work/out"; then
    ok "--host refuses a GCC 16 native prefix for the default 12.4"
else
    fail "--host with a GCC 16 native prefix: exit $rc"; sed 's/^/    /' "$work/out"
fi
run "$work/p12" --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 0 ] && expect cross prefix "$work/p12.win/ppu"; then
    ok "control: --host accepts a GCC 12 native prefix"
else
    [ $rc -eq 0 ] || { fail "--host with a GCC 12 native prefix: exit $rc"; sed 's/^/    /' "$work/out"; }
fi

# An installed compiler of unknown version is not an empty prefix.
unknown() { # <dir> <script body>
    mkdir -p "$1/ppu/bin"
    printf '#!/bin/sh\n%s\n' "$2" > "$1/ppu/bin/powerpc64-ps3-elf-gcc"
    chmod +x "$1/ppu/bin/powerpc64-ps3-elf-gcc"
}
unknown "$work/u127" 'exit 127'
unknown "$work/uempty" 'exit 0'
unknown "$work/ujunk" 'echo hello'
unknown "$work/unoexec" 'echo 16.1.0'; chmod -x "$work/unoexec/ppu/bin/powerpc64-ps3-elf-gcc"
for u in u127 uempty ujunk unoexec; do
    if [ $u = unoexec ] && [ -x "$work/$u/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        echo "ppu-gcc-select: skip $u: chmod -x has no effect on this filesystem (Git Bash); runs on Linux"
        continue
    fi
    run "$work/$u" --gcc=16 --print-config
    if [ $rc -eq 1 ] && grep -q "did not report a GCC version" "$work/out"; then
        ok "an installed compiler answering no version ($u) is refused"
    else
        fail "$u: exit $rc"; sed 's/^/    /' "$work/out"
    fi
done
# Same for the native prefix of a cross build.
run "$work/u127" --host=x86_64-w64-mingw32 --print-config
if [ $rc -eq 1 ] && grep -q "native prefix .* did not report a GCC version" "$work/out"; then
    ok "--host refuses a native compiler that answers no version"
else
    fail "--host with an unrunnable native compiler: exit $rc"; sed 's/^/    /' "$work/out"
fi

mkdir -p "$work/build/ppu/keep" "$work/build/ppu-gcc16/keep"
run "$dev" --gcc=16 --clean --print-config
if [ $rc -eq 0 ] && [ -d "$work/build/ppu/keep" ] && [ -d "$work/build/ppu-gcc16/keep" ]; then
    ok "--clean happens after the selection: --print-config removed neither build dir"
else
    fail "--gcc=16 --clean --print-config: exit $rc or a build dir was removed"
fi

[ $status -eq 0 ] && echo "PASS: ppu-gcc-select-test"
exit $status

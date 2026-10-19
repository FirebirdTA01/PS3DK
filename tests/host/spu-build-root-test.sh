#!/usr/bin/env bash
# build-spu-toolchain.sh picks its build root from the compiler (--gcc=16, the
# default, or --gcc=9)
# and the host (--host=...) options, in either order, and --clean removes
# only that root.  Before this test, --clean removed $PS3_BUILD_ROOT/spu (the
# 9.5 root) as soon as it was parsed, so `--clean --gcc=16` and
# `--gcc=16 --clean` both deleted the 9.5 tree and left the 16 tree intact,
# and --host replaced the -gcc16 suffix, so a 9.5 and a 16 cross build shared
# one root.
#
# Checks: --print-config for every option order (native and cross, 9 and
# 16), an unsupported --gcc refused, and a real --clean run (the cheap
# linker-script step) that must remove the selected root and nothing else.
set -u
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
script="$repo_root/scripts/build-spu-toolchain.sh"
fail=0
ok()  { printf 'spu-build-root: ok   %s\n' "$*"; }
bad() { printf 'spu-build-root: FAIL %s\n' "$*"; fail=1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/spu-build-root.XXXXXX")"
trap 'rm -rf "$work"' EXIT
export PS3DEV="$work/ps3dev" PS3_BUILD_ROOT="$work/build"
# Cross mode requires a native SPU compiler of the same GCC release; a stub
# stands in for it, reports the version in $STUB_VER and exits $STUB_RC (0).
mkdir -p "$PS3DEV/spu/bin" && printf '#!/bin/sh\necho "$STUB_VER"\nexit ${STUB_RC:-0}\n' > "$PS3DEV/spu/bin/spu-elf-gcc" && chmod +x "$PS3DEV/spu/bin/spu-elf-gcc"
host=x86_64-w64-mingw32

config() { bash "$script" --print-config "$@" 2>/dev/null | tr -d '\r'; }
expect() { # <want build root suffix> <want clean 0|1> <want gcc_ver> <args...>
    local root=$1 clean=$2 ver=$3; shift 3
    local out; out=$(STUB_VER=$ver config "$@")
    local b c v
    b=$(sed -n 's/^build=//p' <<<"$out"); c=$(sed -n 's/^clean=//p' <<<"$out"); v=$(sed -n 's/^gcc_ver=//p' <<<"$out")
    if [[ "$b" == "$PS3_BUILD_ROOT/$root" && "$c" == "$clean" && "$v" == "$ver" ]]; then
        ok "$* -> $root clean=$clean gcc $ver"
    else
        bad "$* -> build=${b:-?} clean=${c:-?} gcc=${v:-?}, want $root clean=$clean gcc $ver"
    fi
}

expect spu-gcc16           0 16.1.0
expect spu-gcc16           1 16.1.0 --clean
expect spu-gcc16           0 16.1.0 --gcc=16
expect spu-gcc16           1 16.1.0 --clean --gcc=16
expect spu-gcc16           1 16.1.0 --gcc=16 --clean
expect spu                 0 9.5.0  --gcc=9
expect spu                 0 9.5.0  --gcc=9.5
expect spu                 1 9.5.0  --clean --gcc=9
expect spu                 1 9.5.0  --gcc=9 --clean
expect "spu-gcc16-$host"   0 16.1.0 --host=$host
expect "spu-gcc16-$host"   1 16.1.0 --clean --host=$host
expect "spu-$host"         0 9.5.0  --gcc=9 --host=$host
expect "spu-$host"         0 9.5.0  --host=$host --gcc=9
expect "spu-$host"         1 9.5.0  --clean --host=$host --gcc=9
expect "spu-$host"         1 9.5.0  --host=$host --gcc=9 --clean

# A cross build over a native compiler of the other release is refused.
for pair in "16.1.0 --gcc=9" "9.5.0 --gcc=16" "9.5.0 "; do
    v=${pair%% *}; a=${pair#* }
    if STUB_VER=$v bash "$script" --print-config --host=$host $a >/dev/null 2>&1; then
        bad "cross ${a:-(default)} over a native $v compiler was accepted"
    else
        ok "cross ${a:-(default)} over a native $v compiler refused"
    fi
done
# The right version printed by a probe that then fails is refused too.
if STUB_VER=16.1.0 STUB_RC=23 bash "$script" --print-config --host=$host >/dev/null 2>&1; then
    bad "cross over a native compiler whose version probe exits 23 was accepted"
else
    ok "cross over a native compiler whose version probe exits 23 refused"
fi

if bash "$script" --print-config --gcc=12 >/dev/null 2>&1; then
    bad "--gcc=12 was accepted"
else
    ok "--gcc=12 refused"
fi

# A real --clean: only the selected root goes.
for d in spu spu-gcc16 "spu-$host" "spu-gcc16-$host"; do mkdir -p "$PS3_BUILD_ROOT/$d" && touch "$PS3_BUILD_ROOT/$d/marker"; done
if bash "$script" --clean --only=linker-script > "$work/clean.log" 2>&1; then
    # The script recreates its (empty) build root after cleaning, so judge by
    # which roots still hold their marker file.
    left=$(cd "$PS3_BUILD_ROOT" && for d in */; do [ -e "$d/marker" ] && echo "${d%/}"; done | sort | tr '\n' ' ')
    want=$(printf '%s\n' spu "spu-$host" "spu-gcc16-$host" | sort | tr '\n' ' ')
    [[ "$left" == "$want" ]] && ok "--clean (default GCC 16) emptied only spu-gcc16 (markers left in: $left)" \
        || bad "--clean: markers left in $left (want: $want)"
else
    bad "--clean --only=linker-script failed: $(tail -n 3 "$work/clean.log")"
fi

if [[ $fail -ne 0 ]]; then printf 'FAIL: spu-build-root-test\n'; exit 1; fi
printf 'PASS: spu-build-root-test\n'

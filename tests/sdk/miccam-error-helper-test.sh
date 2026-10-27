#!/usr/bin/env bash
# CELL_CAMERA_MAKE_ERROR / CELL_MICIN_MAKE_ERROR (miccam-error-helper-test.c).
# The header-shape checks always run.  With a PPU toolchain (--ps3dev PREFIX,
# or PS3DEV / PS3DK in the environment) the test also compiles the probe as
# gnu99, c11 and c++17 for ILP32 and LP64 under -Wall -Wextra -Werror, and
# requires the negative control to fail on its assertion in both ABIs.
#
# usage: miccam-error-helper-test.sh [--ps3dev PREFIX]
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
ps3dev=${PS3DEV:-${PS3DK:-}}
while (($#)); do
    case $1 in
        --ps3dev) ps3dev=$2; shift 2 ;;
        *) echo "usage: $0 [--ps3dev PREFIX]" >&2; exit 2 ;;
    esac
done
inc="$root/sdk/include"
src="$root/tests/sdk/miccam-error-helper-test.c"
status=0

# Each helper is defined exactly once, in its own header.
for pair in cell/camera.h:CELL_CAMERA_MAKE_ERROR cell/mic/error.h:CELL_MICIN_MAKE_ERROR; do
    header=${pair%%:*} name=${pair#*:}
    hits=$(grep -rlE "^#define[[:space:]]+$name\b" "$inc" || true)
    if [[ $hits == "$inc/$header" ]]; then
        echo "miccam-error-helper: ok   $name defined only in $header"
    else
        echo "miccam-error-helper: FAIL $name definitions: ${hits:-none}"; status=1
    fi
done

cc=; cxx=
if [[ -n $ps3dev ]]; then
    for suffix in '' .exe; do
        [[ -z $cc && -f $ps3dev/ppu/bin/powerpc64-ps3-elf-gcc$suffix ]] && cc=$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc$suffix
        [[ -z $cxx && -f $ps3dev/ppu/bin/powerpc64-ps3-elf-g++$suffix ]] && cxx=$ps3dev/ppu/bin/powerpc64-ps3-elf-g++$suffix
    done
fi
if [[ -z $cc || -z $cxx ]]; then
    echo "miccam-error-helper: SKIP target compiles (no PPU toolchain; set PS3DEV or --ps3dev)"
    ((status == 0)) && echo "PASS: miccam-error-helper-test (shape only)"
    exit $status
fi
"$cc" --version | head -n 1

work=$(mktemp -d)
compile() {  # compile LANG STD ABI EXTRA...
    local lang=$1 std=$2 abi=$3; shift 3
    local driver=$cc; [[ $lang == c++ ]] && driver=$cxx
    local abiflag=(); [[ $abi == lp64 ]] && abiflag=(-mlp64)
    "$driver" -x "$lang" "-std=$std" "${abiflag[@]}" -O2 -Wall -Wextra -Werror \
        -I"$inc" -I"$ps3dev/ppu/include" "$@" -c "$src" -o "$work/probe.o" 2>&1
}
for abi in ilp32 lp64; do
    for mode in c:gnu99 c:c11 c++:c++17; do
        lang=${mode%%:*} std=${mode#*:}
        if out=$(compile "$lang" "$std" "$abi"); then
            echo "miccam-error-helper: ok   $lang $std $abi"
        else
            echo "miccam-error-helper: FAIL $lang $std $abi"; printf '%s\n' "$out" | head -n 8; status=1
        fi
    done
    if out=$(compile c c11 "$abi" -DERROR_HELPER_NEGATIVE); then
        echo "miccam-error-helper: FAIL negative control compiled ($abi)"; status=1
    elif grep -q 'negative control: wrong status must not match' <<<"$out"; then
        echo "miccam-error-helper: ok   negative control rejected ($abi)"
    else
        echo "miccam-error-helper: FAIL negative control failed for another reason ($abi)"
        printf '%s\n' "$out" | head -n 8; status=1
    fi
done

((status == 0)) && echo "PASS: miccam-error-helper-test"
exit $status

#!/usr/bin/env bash
# CELL_CAMERA_MAKE_ERROR / CELL_MICIN_MAKE_ERROR (miccam-error-helper-test.c).
# The header-shape checks always run: each helper must have exactly one
# definition line, in its own header, and the check itself must reject a
# same-header duplicate and a definition in the wrong header (controls run on
# copies of the two headers).  With a PPU toolchain (--ps3dev PREFIX,
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

# Each helper has exactly one definition line under the include tree, and it
# is in its own header.  Counts lines, not files; allows the whitespace C
# permits before and after '#'.
shape_check() {  # shape_check INCLUDE_DIR; prints one line per helper, returns 1 on failure
    local dir=$1 rc=0 pair header name hits count
    for pair in cell/camera.h:CELL_CAMERA_MAKE_ERROR cell/mic/error.h:CELL_MICIN_MAKE_ERROR; do
        header=${pair%%:*} name=${pair#*:}
        hits=$(grep -rnE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$name([^A-Za-z0-9_]|$)" "$dir" || true)
        count=$(printf '%s' "$hits" | grep -c . || true)
        if [[ $count -eq 1 && ${hits%%:*} == "$dir/$header" ]]; then
            echo "miccam-error-helper: ok   $name defined once, in $header"
        elif [[ $count -eq 0 ]]; then
            echo "miccam-error-helper: FAIL $name has no definition"; rc=1
        elif [[ $count -gt 1 ]]; then
            echo "miccam-error-helper: FAIL $name defined $count times:"
            while IFS= read -r line; do echo "    ${line#"$dir"/}"; done <<<"$hits"; rc=1
        else
            echo "miccam-error-helper: FAIL $name defined outside $header: ${hits#$dir/}"; rc=1
        fi
    done
    return $rc
}
shape_check "$inc" || status=1

# The shape check must reject a second definition in the same header and a
# definition in the wrong header; run it on copies of the two headers.
fixture=$(mktemp -d)
for case in clean duplicate wrong-header; do
    mkdir -p "$fixture/$case/cell/mic"
    cp "$inc/cell/camera.h" "$fixture/$case/cell/camera.h"
    cp "$inc/cell/mic/error.h" "$fixture/$case/cell/mic/error.h"
done
printf '#  define CELL_CAMERA_MAKE_ERROR(status) 0\n' >> "$fixture/duplicate/cell/camera.h"
grep -vE 'CELL_MICIN_MAKE_ERROR|CELL_ERROR_MAKE_ERROR\(CELL_ERROR_FACILITY_MICCAM, status\)$' \
    "$inc/cell/mic/error.h" > "$fixture/wrong-header/cell/mic/error.h"
printf '#define CELL_MICIN_MAKE_ERROR(status) CELL_ERROR_MAKE_ERROR(CELL_ERROR_FACILITY_MICCAM, status)\n' \
    >> "$fixture/wrong-header/cell/camera.h"
expect() {  # expect CASE WANT_RC DIAGNOSTIC
    local out rc
    out=$(shape_check "$fixture/$1") && rc=0 || rc=$?
    if [[ $rc -eq $2 ]] && grep -qF -- "$3" <<<"$out"; then
        echo "miccam-error-helper: ok   shape control $1"
    else
        echo "miccam-error-helper: FAIL shape control $1 (rc=$rc)"; printf '    %s\n' "$out"; status=1
    fi
}
expect clean 0 'CELL_CAMERA_MAKE_ERROR defined once, in cell/camera.h'
expect duplicate 1 'CELL_CAMERA_MAKE_ERROR defined 2 times'
expect wrong-header 1 'CELL_MICIN_MAKE_ERROR defined outside cell/mic/error.h'

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

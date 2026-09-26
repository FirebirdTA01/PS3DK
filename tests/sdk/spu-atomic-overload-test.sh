#!/usr/bin/env bash
# Compile the actual SPU atomic header in C99/C11/C++98/C++17. All output
# and diagnostics are retained; this script never changes the SDK prefix.
# --headers permits a frozen installed-header RED control.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
ps3dev=${PS3DEV:-}
headers="$root/sdk/include-spu"
out=
while (($#)); do
    case $1 in
        --ps3dev|--headers|--output)
            if (($# < 2)) || [[ -z $2 ]]; then
                echo "missing value for $1" >&2; exit 2
            fi
            case $1 in
                --ps3dev) ps3dev=$2 ;;
                --headers) headers=$2 ;;
                --output) out=$2 ;;
            esac
            shift 2 ;;
        *) echo "usage: $0 [--ps3dev PREFIX] [--headers INCLUDE_DIR] [--output NEW_DIR]" >&2; exit 2 ;;
    esac
done
if [[ -z $ps3dev ]]; then
    echo 'spu-atomic-overload: SKIP (set PS3DEV or --ps3dev)'
    exit 0
fi
tool="$ps3dev/spu/bin/spu-elf"
for suffix in gcc g++ objdump; do
    [[ -x "$tool-$suffix" ]] || { echo "missing tool: $tool-$suffix" >&2; exit 2; }
done
[[ -f "$headers/cell/atomic.h" ]] || { echo "missing SPU atomic header in $headers" >&2; exit 2; }
if [[ -n $out ]]; then mkdir -- "$out"; else out=$(mktemp -d); fi
out=$(cd "$out" && pwd)
exec > >(tee "$out/run.log") 2>&1
echo "spu-atomic-overload: output=$out"
printf 'headers=%q\nps3dev=%q\n' "$headers" "$ps3dev"
date -u +'%Y-%m-%dT%H:%M:%SZ'
sha256sum "$0" "$root/tests/sdk/spu-atomic-overload-test.c" \
    "$headers/cell/atomic.h" "$tool-gcc" "$tool-g++" "$tool-objdump" > "$out/inputs.sha256"
"$tool-gcc" --version > "$out/compiler-version.txt"
ulimit -c 0
ulimit -v 1048576
status=0
for standard in c99 c11 c++98 c++17; do
    compiler="$tool-gcc"
    language=c
    if [[ $standard == c++* ]]; then compiler="$tool-g++"; language=c++; fi
    cmd=("$compiler" -x "$language" -std="$standard" -O2 -Wall -Wextra -Werror
         -I"$headers" -MD -MF "$out/$standard.d"
         -c "$root/tests/sdk/spu-atomic-overload-test.c" -o "$out/$standard.o")
    printf '%q ' "${cmd[@]}"; printf '\n'
    if timeout 60 "${cmd[@]}" > "$out/$standard.log" 2>&1; then
        if "$tool-objdump" -dr "$out/$standard.o" > "$out/$standard.disassembly"; then
            echo "PASS $standard"
        else
            echo "FAIL $standard: objdump"; status=1
        fi
    else
        rc=$?
        echo "FAIL $standard: compiler exit $rc (see $out/$standard.log)"
        status=1
    fi
done
echo "spu-atomic-overload: status=$status"
exit "$status"

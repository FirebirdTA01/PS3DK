#!/usr/bin/env bash
# Codegen guard for GCC patch 0082: a loop vectorised with factor 1 does not
# realign a splat load with one lvsr mask.
#
# tests/regression/vect-realign-splat/functions.c at -O3 is vectorised as an
# SLP store group of 16 lanes fed by rows[y].  Before 0082 the load of
# rows[y] took the optimized explicit realignment (lvsr of the negated base,
# then vperm of two lvx) although it advances 4 bytes per iteration, so rows 1
# and 2 read the stack slot after the table.  Rows compile the function at
# -O2 and -O3 in both data models and refuse any lvsr in vect_realign_fill; a
# synthetic listing with lvsr must be refused by the scanner (red control).
# The runtime rows vect-realign-splat-{ilp32,lp64} check the values.
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-vect-realign-codegen-test.sh [--ps3dev DIR]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../regression/vect-realign-splat/functions.c"
ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-vect-realign-codegen: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-vect-realign-codegen: FAIL: no PPU compiler under $ps3dev"; exit 1; }
[ -f "$src" ] || { echo "ppu-vect-realign-codegen: FAIL: missing $src"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/scan.py" <<'PYEOF'
import re, sys
on = False; n = 0; bad = 0
for line in open(sys.argv[1]):
    if re.match(r"^(\.L)?\.?vect_realign_fill:", line):
        on = True; continue
    if on and re.match(r"^\s*\.size", line):
        on = False; continue
    if on and re.match(r"^\s+[a-z]", line) and not re.match(r"^\s+\.", line):
        n += 1
        if re.match(r"^\s+lvsr\s", line):
            print("  realignment mask:", line.strip()); bad = 1
if n == 0:
    print("  vect_realign_fill not found"); bad = 1
sys.exit(bad)
PYEOF

status=0
printf 'vect_realign_fill:\n\tsubfic 7,1,-368\n\tlvsr 12,0,7\n\t.size vect_realign_fill,8\n' > "$work/red.s"
printf 'vect_realign_fill:\n\tlvx 0,0,9\n\tstvx 0,0,3\n\t.size vect_realign_fill,8\n' > "$work/green.s"
if ! python3 "$work/scan.py" "$work/red.s" >/dev/null 2>&1 && python3 "$work/scan.py" "$work/green.s" >/dev/null 2>&1; then
    echo "ppu-vect-realign-codegen: ok   scanner controls (lvsr refused, aligned accepted)"
else
    echo "ppu-vect-realign-codegen: FAIL scanner controls"; status=1
fi

for abi in "" -mlp64; do
    for opt in -O2 -O3; do
        label="${abi:-ilp32} $opt"
        rm -f "$work/t.s"
        if ! out=$("$cc" $abi $opt -mcpu=cell -S "$src" -o "$work/t.s" 2>&1); then
            echo "ppu-vect-realign-codegen: FAIL $label: compile"; printf '%s\n' "$out" | head -5; status=1; continue
        fi
        if res=$(python3 "$work/scan.py" "$work/t.s" 2>&1); then
            echo "ppu-vect-realign-codegen: ok   $label"
        else
            echo "ppu-vect-realign-codegen: FAIL $label"; printf '%s\n' "$res"; status=1
        fi
    done
done
[ $status -eq 0 ] && echo "ppu-vect-realign-codegen: PASS"
exit $status

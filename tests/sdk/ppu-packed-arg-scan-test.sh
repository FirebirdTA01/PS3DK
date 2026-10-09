#!/usr/bin/env bash
# Guard for scripts/ppu-packed-arg-scan.py, the check for PPU code that uses
# a packed by-value aggregate register (std::string_view and friends) as an
# address.
#
# The fixtures are assembly, not C++, so the rows keep their meaning once the
# compiler stops emitting the shape:
#   red_copy   splits r4 (sradi 32), then loads through `mr r10,r4`   -> hit
#   red_index  splits r4, then indexes with the raw r4 (lbzx)          -> hit
#   green      splits r4, loads through `clrldi r10,r4,32`             -> clean
#   green_call raw r4 after a call (r4 is clobbered there)             -> clean
# Rows: the object alone and inside an archive must both report exactly the
# two red functions and exit 1; a green-only object must exit 0.
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-packed-arg-scan-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-packed-arg-scan: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
bin="$ps3dev/ppu/bin"
as="$bin/powerpc64-ps3-elf-as"; ar="$bin/powerpc64-ps3-elf-ar"; od="$bin/powerpc64-ps3-elf-objdump"
for t in "$as" "$ar" "$od"; do
    { [ -x "$t" ] || [ -x "$t.exe" ]; } || { echo "ppu-packed-arg-scan: FAIL: missing $t"; exit 1; }
done
here="$(cd "$(dirname "$0")" && pwd)"
scan="$here/../../scripts/ppu-packed-arg-scan.py"
py="$(command -v python3 || command -v python)"
[ -n "$py" ] || { echo "ppu-packed-arg-scan: FAIL: no python"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/red.s" <<'EOF'
	.section .text.red_copy,"ax",@progbits
	.globl red_copy
red_copy:
	sradi 9,4,32
	mr 10,4
	lbz 3,0(10)
	add 3,3,9
	blr
	.section .text.red_index,"ax",@progbits
	.globl red_index
red_index:
	srdi 9,4,32
	li 10,1
	lbzx 3,4,10
	add 3,3,9
	blr
EOF
cat > "$work/green.s" <<'EOF'
	.section .text.green,"ax",@progbits
	.globl green
green:
	sradi 9,4,32
	clrldi 10,4,32
	lbz 3,0(10)
	add 3,3,9
	blr
	.section .text.green_call,"ax",@progbits
	.globl green_call
green_call:
	sradi 9,4,32
	bl green
	lbz 3,0(4)
	blr
EOF

fail=0
row() { # name expected_rc expected_hits files...
    local name="$1" want_rc="$2" want_hits="$3"; shift 3
    local out rc hits
    out="$("$py" "$scan" --objdump "$od" "$@" 2>/dev/null)"; rc=$?
    hits="$(printf '%s\n' "$out" | grep -o 'red_[a-z]*\|green[a-z_]*' | sort -u | tr '\n' ' ')"
    if [ "$rc" -eq "$want_rc" ] && [ "$hits" = "$want_hits" ]; then
        echo "ppu-packed-arg-scan: ok   $name"
    else
        echo "ppu-packed-arg-scan: FAIL $name: rc=$rc (want $want_rc) hits='$hits' (want '$want_hits')"
        printf '%s\n' "$out"
        fail=1
    fi
}

"$as" -a64 -mppc64 "$work/red.s" -o "$work/red.o" && "$as" -a64 -mppc64 "$work/green.s" -o "$work/green.o" \
    || { echo "ppu-packed-arg-scan: FAIL: assembling fixtures"; exit 1; }
"$ar" rc "$work/both.a" "$work/red.o" "$work/green.o" || { echo "ppu-packed-arg-scan: FAIL: ar"; exit 1; }

row "object, red + green" 1 "red_copy red_index " "$work/red.o" "$work/green.o"
row "archive"             1 "red_copy red_index " "$work/both.a"
row "green only"          0 ""                    "$work/green.o"

[ "$fail" -eq 0 ] && echo "ppu-packed-arg-scan: PASS"
exit "$fail"

#!/usr/bin/env bash
# Codegen guard for GCC patch 0064: every floating-point load or store keeps
# a displacement that is a multiple of 4.
#
# Under the 64-bit address model (0059) the vectoriser's realignment IV is
# base+15 and GCC folded the +1 back into the displacement: a clamp loop over
# 8-byte {float; 4 bytes} records compiled to `stfs 0,1(r10)` / `stfs
# 0,9(r10)`.  Those addresses are aligned, but a post-link check of the
# binary (which applications run: a misaligned stfs ends the PPU thread on a
# PS3) cannot tell them from misaligned ones.  Rows compile the loop at -O2,
# -O3 and -Os in both data models; a synthetic odd displacement must be
# refused by the scanner (red control).
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-fp-displacement-codegen-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-fp-displacement-codegen: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-fp-displacement-codegen: FAIL: no PPU compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.c" <<'EOF'
struct stop { float offset; unsigned char r, g, b, a; };
void clamp_stops (struct stop *s, int n)
{
  for (int i = 0; i < n; ++i)
    {
      float o = s[i].offset;
      s[i].offset = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
    }
}
double sum_pairs (const double *p, int n)
{
  double t = 0;
  for (int i = 0; i < n; ++i)
    t += p[2 * i + 1];
  return t;
}
EOF

cat > "$work/scan.py" <<'EOF'
import re, sys
bad = 0
fpop = re.compile(r"^\s+((?:lf|stf)(?:s|d)u?)\s+\d+,\s*(-?\d+)\(\d+\)")
for line in open(sys.argv[1]):
    m = fpop.match(line)
    if m and int(m.group(2)) % 4:
        print("  odd FP displacement:", line.strip()); bad = 1
sys.exit(bad)
EOF

status=0
printf '\tstfs 0,1(10)\n' > "$work/red.s"
printf '\tstfs 0,16(10)\n\tlfd 1,-8(1)\n' > "$work/green.s"
if ! python3 "$work/scan.py" "$work/red.s" >/dev/null 2>&1 && python3 "$work/scan.py" "$work/green.s" >/dev/null 2>&1; then
    echo "ppu-fp-displacement-codegen: ok   scanner controls (odd refused, aligned accepted)"
else
    echo "ppu-fp-displacement-codegen: FAIL scanner controls"; status=1
fi

for abi in "" -mlp64; do
    for opt in -O2 -O3 -Os; do
        label="${abi:-ilp32} $opt"
        if ! out=$("$cc" $abi $opt -mcpu=cell -S "$work/t.c" -o "$work/t.s" 2>&1); then
            echo "ppu-fp-displacement-codegen: FAIL $label: compile"; printf '%s\n' "$out" | head -5; status=1; continue
        fi
        if res=$(python3 "$work/scan.py" "$work/t.s" 2>&1); then
            echo "ppu-fp-displacement-codegen: ok   $label"
        else
            echo "ppu-fp-displacement-codegen: FAIL $label"; printf '%s\n' "$res"; status=1
        fi
    done
done
[ $status -eq 0 ] && echo "ppu-fp-displacement-codegen: PASS"
exit $status

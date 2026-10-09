#!/usr/bin/env bash
# Codegen guard for GCC patch 0061: no floating-point load or store through
# memory the compiler cannot prove aligned to the access size.
#
# On a PS3 an lfs/lfd/stfs/stfd at a misaligned address raises an alignment
# interrupt that silently ends the PPU thread (the process keeps running;
# RPCS3 does not model it).  Before 0061, `float f; memcpy (&f, p, 4)` from a
# char pointer compiled at -O2 to a single `lfs` from p, and packed-struct
# float members could do the same.
#
# Rows compile one TU, in both data models, at -O0, -O2 and -Os, and read
# the assembly:
#   - in each function that touches under-aligned float/double memory, every
#     lfs/lfd/stfs/stfd must address the stack (base r1, or r31 at -O0 where
#     it is the frame pointer): the value
#     travels through integer registers and an aligned stack slot;
#   - control: a plain `*p` load from a `const float *` and a `double *`
#     must still be one direct lfs / lfd from the pointer (base != 1), so the
#     fix costs aligned code nothing.
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-fp-alignment-codegen-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-fp-alignment-codegen: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-fp-alignment-codegen: FAIL: no PPU compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.c" <<'EOF'
#include <string.h>
#pragma pack(push, 1)
struct packed { char tag; float f; double d; };
#pragma pack(pop)
struct holder { int pad; char raw[64]; };

float memcpy_f (const unsigned char *b) { float f; memcpy (&f, b, 4); return f; }
double memcpy_d (const unsigned char *b) { double d; memcpy (&d, b, 8); return d; }
void memcpy_fstore (unsigned char *b, float v) { memcpy (b, &v, 4); }
void memcpy_dstore (unsigned char *b, double v) { memcpy (b, &v, 8); }
float packed_f (const struct packed *p) { return p->f; }
double packed_d (const struct packed *p) { return p->d; }
void packed_fstore (struct packed *p, float v) { p->f = v; }
void packed_dstore (struct packed *p, double v) { p->d = v; }
/* A misaligned offset inside an aligned object: the shape that compiled to
   lfs at a fixed odd offset in a real engine.  */
float offset_f (const struct holder *h) { float f; memcpy (&f, h->raw + 1, 4); return f; }

float aligned_f (const float *p) { return *p; }
double aligned_d (const double *p) { return *p; }
EOF

cat > "$work/scan.py" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
# Stack bases: r1, and r31 at -O0, where it is the frame pointer.
stack = {"1", "31"} if sys.argv[2] == "-O0" else {"1"}
under = {"memcpy_f", "memcpy_d", "memcpy_fstore", "memcpy_dstore", "packed_f",
         "packed_d", "packed_fstore", "packed_dstore", "offset_f"}
aligned = {"aligned_f": "lfs", "aligned_d": "lfd"}
funcs, cur = {}, None
for line in text.splitlines():
    m = re.match(r"^\.L\.(\w+):", line)
    if m:
        cur = m.group(1); funcs[cur] = []; continue
    if cur and re.match(r"^\s+\.size\s", line):
        cur = None; continue
    if cur:
        funcs[cur].append(line)
fp = re.compile(r"^\s+(lfs|lfd|stfs|stfd)(u?x?)\s+\d+,\s*(-?\d+)?\((\d+)\)")
bad = 0
for name in sorted(under):
    if name not in funcs:
        print(f"  missing function {name}"); bad = 1; continue
    for l in funcs[name]:
        m = fp.match(l)
        if m and m.group(4) not in stack:
            print(f"  {name}: FP access not through the stack: {l.strip()}"); bad = 1
for name, op in aligned.items():
    hits = [l for l in funcs.get(name, []) if (m := fp.match(l)) and m.group(1) == op and m.group(4) not in stack]
    if not hits:
        print(f"  {name}: expected a direct {op} from the pointer (control)"); bad = 1
sys.exit(bad)
EOF

status=0
for abi in "" -mlp64; do
    for opt in -O0 -O2 -Os; do
        label="${abi:-ilp32} $opt"
        if ! out=$("$cc" $abi $opt -S "$work/t.c" -o "$work/t.s" 2>&1); then
            echo "ppu-fp-alignment-codegen: FAIL $label: compile"; printf '%s\n' "$out" | head -5; status=1; continue
        fi
        if res=$(python3 "$work/scan.py" "$work/t.s" "$opt" 2>&1); then
            echo "ppu-fp-alignment-codegen: ok   $label"
        else
            echo "ppu-fp-alignment-codegen: FAIL $label"; printf '%s\n' "$res"; status=1
        fi
    done
done
[ $status -eq 0 ] && echo "ppu-fp-alignment-codegen: PASS"
exit $status

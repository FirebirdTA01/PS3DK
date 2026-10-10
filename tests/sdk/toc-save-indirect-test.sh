#!/usr/bin/env bash
# An indirect call must save the TOC after the last call that precedes it
# (GCC patch 0060), in the width the PRX import stubs use (0063).  A stub
# stores the caller's TOC with a 4-byte stw at 40(r1).  A prologue TOC save
# (-msave-toc-indirect) followed by a stub call and then an indirect call
# reloaded a TOC whose high word was the stub's copy
# (samples/spurs/sync2-objects).  Saving with std at each call still failed
# when the function pointer is itself an import: the stub runs between the
# std and the ld and stw's into the high word, so the ld returns the TOC in
# both halves (tests/regression/ilp32-pointers: Narrowing error at
# 0x00069b00_00061c98 under the RPCS3 static interpreter).
#
# Compiles a function that calls an external function, then a function
# pointer, at -O0/-O2/-Os in both data models, and checks that every bctrl
# is preceded by a TOC save with no call between the save and the bctrl,
# and that the slot is only accessed in the pointer width (stw/lwz for
# ILP32, std/ld for LP64).  Red controls: the v0.20 candidate 809a6211 (no
# per-call save) and the job-57 v0.23 candidate (std/ld in ILP32).
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: toc-save-indirect-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "toc-save-indirect: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "toc-save-indirect: FAIL: no compiler at $cc"; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/ts.c" <<'EOF'
extern int printf (const char *, ...);
typedef void (*row_fn) (void);
int counter;
void run_rows (row_fn *rows, int n)
{
  for (int i = 0; i < n; i++)
    {
      printf ("row %d\n", i);
      rows[i] ();
      counter++;
    }
}
EOF

status=0
for model in "" -mlp64; do
    for opt in -O0 -O2 -Os; do
        label="${model:-ILP32} $opt"
        if ! "$cc" ${EXTRA_CFLAGS:-} -mcpu=cell $model $opt -S "$work/ts.c" -o "$work/ts.s" 2> "$work/err.txt"; then
            echo "toc-save-indirect: FAIL $label does not compile"; head -3 "$work/err.txt"; status=1; continue
        fi
        # The slot holds a pointer: stw/lwz for ILP32, std/ld for LP64.
        if [ -n "$model" ]; then st=std; ldi=ld; else st=stw; ldi=lwz; fi
        verdict=$(awk -v st="$st" -v ldi="$ldi" '
            /^\.L\.run_rows:/ { on = 1; next }
            on && /^[ \t]+blr/ { exit }
            !on { next }
            /[ \t](std|stw|ld|lwz) 2,40\(1\)/ {
                op = $1
                if (op != st && op != ldi) wrong++
                if (op == st) saved = 1
                next
            }
            /^[ \t]+bl[ \t]/ { saved = 0; next }
            /^[ \t]+bctrl/ { n++; if (!saved) bad++; saved = 0 }
            END { printf "%d %d %d", n, bad, wrong }' "$work/ts.s")
        set -- $verdict
        if [ "${1:-0}" -ge 1 ] && [ "${2:-0}" -eq 0 ] && [ "${3:-0}" -eq 0 ]; then
            echo "toc-save-indirect: ok   $label TOC saved ($st/$ldi) after the last call before each of $1 bctrl"
        else
            echo "toc-save-indirect: FAIL $label ${2:-?} of ${1:-?} bctrl reload a TOC saved before an intervening call; ${3:-?} slot accesses are not $st/$ldi"
            sed -n '/^\.L\.run_rows:/,/blr/p' "$work/ts.s" | grep -E "std 2|ld 2|lwz 2|bl |bctrl"
            status=1
        fi
    done
done

[ $status -eq 0 ] && echo "toc-save-indirect: PASS" || echo "toc-save-indirect: FAIL"
exit $status

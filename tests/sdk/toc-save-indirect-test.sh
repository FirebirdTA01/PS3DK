#!/usr/bin/env bash
# An indirect call must save the TOC after the last call that precedes it
# (GCC patch 0054).  A PRX import stub stores the caller's TOC with a 4-byte
# stw at 40(r1); with Pmode DImode the TOC is restored with an 8-byte ld, so
# GCC's prologue TOC save (-msave-toc-indirect) followed by a stub call and
# then an indirect call reloads a TOC whose high word is the stub's copy, and
# the next TOC-relative access faults (samples/spurs/sync2-objects: main
# calls printf through a stub, then each row through a function pointer).
# The reference saves the TOC with std right before every bctrl.
#
# Compiles a function that calls an external function, then a function
# pointer, at -O0/-O2/-Os in both data models, and checks that every
# bctrl is preceded by a "std 2,40(1)" with no call between the save and
# the bctrl.  The v0.20 candidate 809a6211 compiler is the red control.
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
        verdict=$(awk '
            /^\.L\.run_rows:/ { on = 1; next }
            on && /^[ \t]+blr/ { exit }
            !on { next }
            /std 2,40\(1\)/ { saved = 1; next }
            /^[ \t]+bl[ \t]/ { saved = 0; next }
            /^[ \t]+bctrl/ { n++; if (!saved) bad++; saved = 0 }
            END { printf "%d %d", n, bad }' "$work/ts.s")
        set -- $verdict
        if [ "${1:-0}" -ge 1 ] && [ "${2:-0}" -eq 0 ]; then
            echo "toc-save-indirect: ok   $label TOC saved after the last call before each of $1 bctrl"
        else
            echo "toc-save-indirect: FAIL $label ${2:-?} of ${1:-?} bctrl reload a TOC saved before an intervening call"
            sed -n '/^\.L\.run_rows:/,/blr/p' "$work/ts.s" | grep -E "std 2|ld 2|lwz 2|bl |bctrl"
            status=1
        fi
    done
done

[ $status -eq 0 ] && echo "toc-save-indirect: PASS" || echo "toc-save-indirect: FAIL"
exit $status

#!/usr/bin/env bash
# Codegen guard: callee-saved GPRs are saved and restored at full register
# width.  Under the cell64lv2 ILP32 hybrid a 64-bit value can live in a
# nonvolatile GPR across a call (a long long, or a by-value aggregate such
# as libstdc++'s ostreambuf_iterator with the pointer in the high word).  A
# prologue that saved it with stw and restored it with lwz dropped the high
# 32 bits; GCC patch 0031 forced DImode saves, and 0059's DImode Pmode
# covers it now.
#
# Compiles the probe below at -O0 to -Os in both data models and checks every
# save/restore of r14-r31 against the stack pointer, and every access to the
# link-register save slot (r0 at 16(r1)): only std/ld may appear, never
# stw/lwz/stmw/lmw (GCC patch 0037 did the same for the LR slot).
# Red controls (embedded): listings with "stw 31,-4(1)", "lwz 30,8(1)" and
# "stw 0,16(1)" must be rejected; a listing with std/ld must pass.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: ilp32-callee-save-width-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# scan <listing>: print each bad save/restore; exit 1 if any, 2 if none checked.
cat > "$work/scan.sh" <<'EOF'
#!/usr/bin/env bash
awk '
  { line = $0; sub(/^[ \t]+/, "", line) }
  line ~ /^(stw|stwu|lwz|stmw|lmw|std|ld|stdu)[ \t]/ {
      split(line, f, /[ \t,()]+/)
      op = f[1]; reg = f[2] + 0; off = f[3]; base = f[4]
      if (base != "1" || op == "stdu" || op == "stwu") next
      if (!((reg >= 14 && reg <= 31) || (reg == 0 && off == "16"))) next
      checked++
      if (op == "stw" || op == "lwz" || op == "stmw" || op == "lmw") { print "  narrow " line; bad++ }
  }
  END { if (bad) exit 1; if (!checked) exit 2 }' "$1"
EOF
chmod +x "$work/scan.sh"
status=0
printf '\tstd 31,-8(1)\n\tbl g\n\tld 31,-8(1)\n' > "$work/good.s"
printf '\tstw 31,-4(1)\n\tbl g\n\tlwz 31,-4(1)\n' > "$work/red1.s"
printf '\tstd 30,8(1)\n\tbl g\n\tlwz 30,8(1)\n' > "$work/red2.s"
printf '\tmflr 0\n\tstw 0,16(1)\n\tbl g\n\tlwz 0,16(1)\n\tmtlr 0\n' > "$work/red3.s"
"$work/scan.sh" "$work/good.s" > /dev/null; g=$?
"$work/scan.sh" "$work/red1.s" > /dev/null; r1=$?
"$work/scan.sh" "$work/red2.s" > /dev/null; r2=$?
"$work/scan.sh" "$work/red3.s" > /dev/null; r3=$?
if [ $g -eq 0 ] && [ $r1 -eq 1 ] && [ $r2 -eq 1 ] && [ $r3 -eq 1 ]; then
    echo "ilp32-callee-save-width: ok   controls: std/ld accepted; stw/lwz callee and LR-slot saves rejected"
else
    echo "ilp32-callee-save-width: FAIL controls (good $g, red1 $r1, red2 $r2, red3 $r3)"; status=1
fi

if [ -z "$ps3dev" ]; then
    echo "ilp32-callee-save-width: SKIP compile leg (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "PASS: ilp32-callee-save-width-codegen-test (controls only)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ilp32-callee-save-width: FAIL: no PPU compiler under $ps3dev"; exit 1; }
extra=${EXTRA_CFLAGS:-}
cat > "$work/probe.c" <<'EOF'
extern void g(void);
extern void h(unsigned long long);
struct pair { unsigned hi, lo; };
extern void use(struct pair);
/* A 64-bit value live across a call sits in a nonvolatile GPR. */
unsigned long long keep(unsigned long long x) { g(); return x + 1; }
/* Several, so more than one nonvolatile register is saved. */
unsigned long long keep3(unsigned long long a, unsigned long long b, unsigned long long c)
{ g(); h(a); g(); return a ^ b ^ c; }
/* A by-value 8-byte aggregate with the first word in the high half. */
void keep_pair(struct pair p) { g(); use(p); g(); use(p); }
EOF
for abi in "" -mlp64; do for opt in -O0 -O1 -O2 -O3 -Os; do
    label="${abi:-ilp32} $opt"
    rm -f "$work/p.s"
    # shellcheck disable=SC2086
    if ! "$cc" $extra $abi $opt -S "$work/probe.c" -o "$work/p.s" 2> "$work/err" || [ ! -s "$work/p.s" ]; then
        echo "ilp32-callee-save-width: FAIL $label: compile failed"; sed 's/^/    /' "$work/err" | head -3; status=1; continue
    fi
    out=$("$work/scan.sh" "$work/p.s"); rc=$?
    case $rc in
        0) echo "ilp32-callee-save-width: ok   $label" ;;
        2) echo "ilp32-callee-save-width: FAIL $label: no callee save/restore found to check"; status=1 ;;
        *) echo "ilp32-callee-save-width: FAIL $label"; printf '%s\n' "$out" | head -6; status=1 ;;
    esac
done; done
[ $status -eq 0 ] && echo "PASS: ilp32-callee-save-width-codegen-test"
exit $status

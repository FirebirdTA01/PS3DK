#!/usr/bin/env bash
# Codegen guard for GCC patch 0067: the PPU stack protector compiles in both
# data models, with guard and canary accesses of pointer width.  Before 0067
# every protected function in ILP32 stopped with "unrecognizable insn" (the
# patterns followed TARGET_64BIT instead of the 4-byte pointer mode).
#
# Compiles a probe (arrays, alloca, a VLA, a struct with an array, C++ with
# std::string) with -fstack-protector, -strong and -all, ILP32 and LP64,
# -O0/-O1/-O2/-O3/-Os.  Each row removes its output first, requires exit 0
# and a non-empty listing, then checks every protector sequence:
#   set   L r,guard ; S r,slot ; li r,0
#   test  L a,slot ; L b,guard ; xor. a,a,b (or cmpl a,b) ; li b,0
# L/S must be lwz/stw in ILP32 and ld/std in LP64; every listing must
# protect something, have a test for every set and call __stack_chk_fail.
# A listing built without a protector flag must contain no sequence.
# Controls (embedded) always run: a correct ILP32 listing passes; an ld in
# ILP32, a missing test and a missing __stack_chk_fail call are rejected,
# and the LP64 check rejects the ILP32 listing.
# The runtime half is tests/regression/ssp.
#
# Skips the compile leg without PS3DEV (CI has no PPU compiler).
# usage: ppu-stack-protector-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
T=ppu-stack-protector
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
py=$(command -v python3 || command -v python) || { echo "$T: FAIL: no python"; exit 1; }
status=0

cat > "$work/check.py" <<'EOF'
import re, sys
path, abi, want = sys.argv[1], sys.argv[2], sys.argv[3]
lp64 = abi == 'lp64'
LOAD, STORE, CMPL = ('ld', 'std', 'cmpld') if lp64 else ('lwz', 'stw', 'cmplw')
ins = []
for line in open(path):
    s = line.strip()
    if not s or s.startswith(('.', '#')) or s.endswith(':'):
        continue
    m = re.match(r'([a-z.]+)\s+(.*)', s)
    if m:
        ins.append((m.group(1), [x.strip() for x in m.group(2).split(',')]))
loads = {'lwz', 'ld', 'lwa', 'lwzx', 'ldx'}
stores = {'stw', 'std', 'stwx', 'stdx'}
nset = ntest = 0
bad = []
for i in range(len(ins) - 2):
    a, b, c = ins[i], ins[i + 1], ins[i + 2]
    if a[0] in loads and b[0] in stores and c[0] == 'li' and c[1] == [a[1][0], '0'] and b[1][0] == a[1][0]:
        nset += 1
        if (a[0], b[0]) != (LOAD, STORE):
            bad.append('set uses %s/%s, expected %s/%s' % (a[0], b[0], LOAD, STORE))
    if i + 3 < len(ins):
        d = ins[i + 3]
        if a[0] in loads and b[0] in loads and d[0] == 'li' and d[1] == [b[1][0], '0'] and (
                (c[0] == 'xor.' and c[1] == [a[1][0], a[1][0], b[1][0]]) or
                (c[0] in ('cmplw', 'cmpld') and c[1][1:] == [a[1][0], b[1][0]])):
            ntest += 1
            if a[0] != LOAD or b[0] != LOAD or c[0] not in ('xor.', CMPL):
                bad.append('test uses %s/%s/%s, expected %s/%s/(xor.|%s)' % (a[0], b[0], c[0], LOAD, LOAD, CMPL))
calls_fail = any(op in ('bl', 'b') and args and args[0].startswith('__stack_chk_fail') for op, args in ins)
if want == 'protected':
    if nset == 0:
        bad.append('no protector sequence')
    if ntest < nset:
        bad.append('%d set(s) but %d test(s)' % (nset, ntest))
    if not calls_fail:
        bad.append('no call to __stack_chk_fail')
elif nset or ntest:
    bad.append('unprotected build has %d set / %d test sequence(s)' % (nset, ntest))
print('set %d test %d%s' % (nset, ntest, ('; ' + '; '.join(sorted(set(bad)))) if bad else ''))
sys.exit(1 if bad else 0)
EOF

# Controls.
printf '\tlwz 9,0(10)\n\tstw 9,28(1)\n\tli 9,0\n\tbl use\n\tlwz 9,28(1)\n\tlwz 10,0(10)\n\txor. 9,9,10\n\tli 10,0\n\tbl __stack_chk_fail\n' > "$work/good.s"
sed '1s/lwz/ld/' "$work/good.s" > "$work/red-ld.s"
sed '/xor/d' "$work/good.s" > "$work/red-notest.s"
sed '/__stack_chk_fail/d' "$work/good.s" > "$work/red-nofail.s"
cr() { "$py" "$work/check.py" "$work/$1" "$2" "$3" > /dev/null; echo $?; }
c="$(cr good.s ilp32 protected)$(cr red-ld.s ilp32 protected)$(cr red-notest.s ilp32 protected)$(cr red-nofail.s ilp32 protected)$(cr good.s lp64 protected)$(cr good.s ilp32 none)"
if [ "$c" = 011111 ]; then
    echo "$T: ok   controls: correct listing accepted; ld in ILP32, missing test, missing __stack_chk_fail, ILP32 widths under the LP64 check and a sequence in an unprotected build rejected"
else
    echo "$T: FAIL controls ($c, expected 011111)"; status=1
fi

if [ -z "$ps3dev" ]; then
    echo "$T: SKIP compile leg (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "PASS: ppu-stack-protector-codegen-test (controls only)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"; cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "$T: FAIL: no PPU compiler under $ps3dev"; exit 1; }
extra=${EXTRA_CFLAGS:-}
cat > "$work/p.c" <<'EOF'
#include <alloca.h>
#include <string.h>
extern void use (void *, unsigned);
void arr (void) { char b[64]; use (b, sizeof b); }
void dyn (unsigned n) { char *p = alloca (n); use (p, n); }
void vla (unsigned n) { short v[n]; use (v, sizeof v); }
struct S { char name[16]; int v; };
int st (const char *s) { struct S t; strcpy (t.name, s); use (&t, sizeof t); return t.v; }
EOF
cat > "$work/p.cc" <<'EOF'
#include <string>
extern void use (void *, unsigned);
int f (const std::string &s) { char b[32]; s.copy (b, sizeof b - 1); b[31] = 0; use (b, 32); return (int) s.size (); }
EOF
for f in p.c p.cc; do
    drv=$cc; std=; [ "$f" = p.cc ] && { drv=$cxx; std=-std=gnu++17; }
    for sp in none -fstack-protector -fstack-protector-strong -fstack-protector-all; do
        flag=$sp; want=protected; [ "$sp" = none ] && { flag=; want=none; }
        for abi in ilp32 lp64; do
            mabi=; [ $abi = lp64 ] && mabi=-mlp64
            for opt in -O0 -O1 -O2 -O3 -Os; do
                label="$f $sp $abi $opt"
                rm -f "$work/o.s" "$work/err"
                # shellcheck disable=SC2086
                "$drv" $extra $mabi $opt $std $flag -S "$work/$f" -o "$work/o.s" 2> "$work/err"; rc=$?
                if [ $rc -ne 0 ] || [ ! -s "$work/o.s" ]; then
                    echo "$T: FAIL $label: compile exit $rc"; grep -m2 -E 'error|internal' "$work/err" | sed 's/^/    /'; status=1; continue
                fi
                out=$("$py" "$work/check.py" "$work/o.s" $abi $want); rc=$?
                if [ $rc -eq 0 ]; then echo "$T: ok   $label: $out"
                else echo "$T: FAIL $label: $out"; status=1; fi
            done
        done
    done
done
[ $status -eq 0 ] && echo "PASS: ppu-stack-protector-codegen-test"
exit $status

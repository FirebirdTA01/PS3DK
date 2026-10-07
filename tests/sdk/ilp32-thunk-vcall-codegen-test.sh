#!/usr/bin/env bash
# Codegen guard for GCC patch 0056: a virtual-base thunk adds the vcall
# offset read from the vtable to `this'.  The offset is a signed displacement
# (normally negative); under ILP32 the pointer lives zero-extended in a 64-bit
# GPR and the add is full width, so the offset must be sign-extended before
# the add.  The rs6000 assembly-thunk path (-O0, -O2 -fno-inline) loaded it
# with lwz and added it zero-extended: `this' reached the target as
# this + 2^32 - k (tests/regression/ilp32-thunk-vcall shows it at run time).
#
# Compiles tests/regression/ilp32-thunk-vcall/thunk.cpp in both data models
# at -O0 and -O2 -fno-inline (the assembly path: each thunk must end in a tail
# branch to .LTHUNKn) and at -O2 and -Os (the generic path), and reads every
# virtual thunk:
#   ILP32: vtable pointer by lwz (an address: zero-extended), vcall offset by
#          lwa, or by lwz followed by extsw of that register, before the add
#          into `this';
#   LP64:  both loads are ld.
# Any other thunk shape fails rather than passes.  Controls (embedded below):
# the v0.20.6 compiler's -O0 thunk must be reported, and so must an extsw on
# the wrong register or after the add; the fixed shape and the generic lwa
# shape must be accepted.
#
# Skips the compiles without PS3DEV (CI has no PPU compiler); run it in the
# release gate.
# usage: ilp32-thunk-vcall-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1plus).
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/scan.py" <<'EOF'
import re, sys

INSN = re.compile(r"^\s+([a-z][a-z0-9.]*)\s*(.*?)\s*$")
LABEL = re.compile(r"^(\S+):")
THUNK = re.compile(r"^\.L\._ZTv")
MEM = re.compile(r"^(\d+),(-?\d+)\((\d+)\)$")
# Instructions whose first operand is not a GPR they write.
NOWRITE = re.compile(r"^(cmp|fcmp|st|b|tw|td|dcb|mt|sync|isync|nop|\.)")

def thunks(path):
    found, cur = [], None
    for raw in open(path):
        line = raw.rstrip("\n")
        if line.startswith("#") or line.lstrip().startswith("#"):
            continue
        m = LABEL.match(line)
        if m:
            if THUNK.match(m.group(1)):
                cur = (m.group(1), [])
                found.append(cur)
            continue
        m = INSN.match(line)
        if m and cur is not None:
            op, args = m.group(1), m.group(2).replace(" ", "")
            if op.startswith("."):
                continue
            cur[1].append((op, args))
            if op in ("b", "blr", "bctr"):
                cur = None
    return found

def judge(name, insns, lp64, asm_path):
    ptr_load = "ld" if lp64 else "lwz"
    i0 = rv = rt = None
    for i, (op, a) in enumerate(insns):
        m = MEM.match(a)
        if op in ("lwz", "lwa", "ld") and m and m.group(3) in ("3", "4") and int(m.group(2)) == 0:
            i0, rv, rt = i, m.group(1), m.group(3)
            if op != ptr_load:
                return "vtable pointer loaded by %s, expected %s" % (op, ptr_load)
            break
    if i0 is None:
        return "no vtable-pointer load from this"
    i1 = ro = None
    for i in range(i0 + 1, len(insns)):
        op, a = insns[i]
        m = MEM.match(a)
        if op in ("lwz", "lwa", "ld") and m and m.group(3) == rv:
            i1, ro, op1 = i, m.group(1), op
            break
        if not NOWRITE.match(op) and a.split(",")[0] == rv:
            return "vtable register overwritten by %s %s" % (op, a)
    if i1 is None:
        return "no vcall-offset load through the vtable pointer"
    if lp64 and op1 != "ld":
        return "LP64 vcall offset loaded by %s" % op1
    if not lp64 and op1 not in ("lwz", "lwa"):
        return "ILP32 vcall offset loaded by %s" % op1
    signed = op1 in ("lwa", "ld")
    for i in range(i1 + 1, len(insns)):
        op, a = insns[i]
        f = a.split(",")
        if op == "add" and f[0] == rt and sorted(f[1:]) == sorted([rt, ro]):
            if not signed:
                return "vcall offset %s added zero-extended (%s then add)" % (ro, op1)
            tail = insns[-1]
            if asm_path and not (tail[0] == "b" and tail[1].startswith(".LTHUNK")):
                return "not the assembly-thunk path (ends %s %s)" % tail
            return None
        if op == "extsw" and f == [ro, ro]:
            signed = True
            continue
        if not NOWRITE.match(op) and f[0] in (ro, rt):
            return "%s overwritten by %s %s before the add" % (f[0], op, a)
    return "vcall offset %s never added to this (r%s)" % (ro, rt)

path, model, path_kind = sys.argv[1], sys.argv[2], sys.argv[3]
found = thunks(path)
findings = 0
for name, insns in found:
    why = judge(name, insns, model == "lp64", path_kind == "asm")
    if why:
        findings += 1
        print("finding: %s: %s" % (name, why))
if len(found) < 2:
    findings += 1
    print("finding: %d virtual thunks, expected 2 (vacuous)" % len(found))
print("%s: %d thunks, %d findings" % (path, len(found), findings))
sys.exit(0 if findings == 0 else 3)
EOF

# The v0.20.6 compiler (unpatched) at -O0, ILP32: the vcall offset is lwz'd
# and added zero-extended.
cat > "$work/red.s" <<'EOF'
.L._ZTv0_n12_N7Derived5probeEv:
	lwz 12,0(3)
	lwz 12,-12(12)
	add 3,3,12
	b .LTHUNK0
.L._ZTv0_n16_N7Derived5valueEv:
	lwz 12,0(3)
	lwz 12,-16(12)
	add 3,3,12
	b .LTHUNK1
EOF
# extsw on the wrong register.
sed 's/^\tadd 3,3,12/\textsw 11,11\n\tadd 3,3,12/' "$work/red.s" > "$work/red-reg.s"
# extsw after the add.
sed 's/^\tadd 3,3,12/\tadd 3,3,12\n\textsw 12,12/' "$work/red.s" > "$work/red-late.s"
# The fixed shape: lwz, extsw, then the add.
sed 's/^\tadd 3,3,12/\textsw 12,12\n\tadd 3,3,12/' "$work/red.s" > "$work/green.s"
# The generic path's shape (-O2/-Os with inlining): lwa, add, clrldi.
cat > "$work/green-lwa.s" <<'EOF'
.L._ZTv0_n16_N7Derived5valueEv:
	lwz 9,0(3)
	lwa 9,-16(9)
	add 3,3,9
	lwa 3,4(3)
	blr
.L._ZTv0_n12_N7Derived5probeEv:
	lwz 9,0(3)
	lwa 9,-12(9)
	add 3,3,9
	rldicl 3,3,0,32
	blr
EOF
status=0
control() {  # <name> <model> <path> <expected exit> <expected text> <label>
    python3 "$work/scan.py" "$work/$1.s" "$2" "$3" > "$work/$1.out"
    local rc=$?
    if [ $rc -eq "$4" ] && grep -q -- "$5" "$work/$1.out"; then
        echo "ilp32-thunk-vcall-codegen: ok   control: $6"
    else
        echo "ilp32-thunk-vcall-codegen: FAIL control: $6 (exit $rc)"
        cat "$work/$1.out"; status=1
    fi
}
control red ilp32 asm 3 "finding: .L._ZTv0_n12_N7Derived5probeEv: vcall offset 12 added zero-extended" \
    "the v0.20.6 -O0 thunk is reported"
control red-reg ilp32 asm 3 "added zero-extended" "an extsw of another register is reported"
control red-late ilp32 asm 3 "added zero-extended" "an extsw after the add is reported"
control red ilp32 generic 3 "added zero-extended" "the zero-extended add is reported on the generic path too"
control green ilp32 asm 0 ": 2 thunks, 0 findings" "lwz + extsw + add is accepted"
control green-lwa ilp32 generic 0 ": 2 thunks, 0 findings" "the generic lwa shape is accepted"
control green-lwa ilp32 asm 3 "not the assembly-thunk path" "a generic thunk does not satisfy an assembly-path row"
control green lp64 asm 3 "vtable pointer loaded by lwz, expected ld" "an LP64 row refuses 32-bit loads"

ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "ilp32-thunk-vcall-codegen: SKIP compiles (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "ilp32-thunk-vcall-codegen: PASS (controls only)"
    exit $status
fi
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
[ -x "$cxx" ] || [ -x "$cxx.exe" ] || { echo "ilp32-thunk-vcall-codegen: FAIL: no compiler at $cxx"; exit 1; }
src="$root/tests/regression/ilp32-thunk-vcall/thunk.cpp"

for model in ilp32 lp64; do
    flag=""; [ $model = lp64 ] && flag=-mlp64
    for opt in "-O0:asm" "-O2 -fno-inline:asm" "-O2:generic" "-Os:generic"; do
        o="${opt%%:*}"; kind="${opt##*:}"
        name="$model$(echo "$o" | tr -d ' ')"
        if ! "$cxx" ${EXTRA_CFLAGS:-} -mcpu=cell -mhard-float $flag $o -S "$src" -o "$work/$name.s"; then
            echo "ilp32-thunk-vcall-codegen: FAIL $model $o: compile failed"; status=1; continue
        fi
        if python3 "$work/scan.py" "$work/$name.s" $model $kind > "$work/$name.out"; then
            echo "ilp32-thunk-vcall-codegen: ok   $model $o ($kind path): every vcall offset is sign-extended before the add"
        else
            echo "ilp32-thunk-vcall-codegen: FAIL $model $o ($kind path)"; cat "$work/$name.out"; status=1
        fi
    done
done

[ $status -eq 0 ] && echo "ilp32-thunk-vcall-codegen: PASS" || echo "ilp32-thunk-vcall-codegen: FAIL"
exit $status

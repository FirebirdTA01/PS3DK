#!/usr/bin/env bash
# --gc-sections keeps functions reached only through compact .opd
# descriptors (binutils patch 0004).  The PPU ABI's 8-byte descriptors
# [entry32, toc32] were invisible to ld's GC marking: a static function
# whose address is taken, or one called with bl against its descriptor,
# lost its section, its descriptor's entry word was written as 0, and the
# bl branched into .opd.
#
# Links a fixture with and without --gc-sections in both ABIs and checks,
# for every function the fixture reaches only by descriptor: the descriptor
# symbol sits in a compact .opd (4-aligned, 8-byte entries, else REFUSE);
# its entry and TOC words are non-zero; the entry lies in an executable
# section; the code there is byte-identical to the same function's code in
# the non-GC link; and no bl/b in main targets .opd.
# Controls: three doctored copies of the GC link (entry word zeroed, two
# entries swapped, main's bl retargeted at a descriptor) must each be
# rejected with exit 1 naming the doctored function.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: opd-gc-sections-test.sh [--ps3dev DIR] [-B DIR]
#   -B DIR is passed to the driver, e.g. a directory holding a candidate ld.
set -u
ps3dev="${PS3DEV:-}"; bdir=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        -B) bdir="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [-B DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "opd-gc-sections: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "opd-gc-sections: FAIL: no compiler at $cc"; exit 1; }
B=(); [ -n "$bdir" ] && B=("-B$bdir")
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Each opd_* function is reached only through its descriptor.  None of
# them branches or touches the TOC, so its code bytes do not depend on
# where the linker places it.
cat > "$work/fixture.c" <<'EOF'
static int __attribute__((noinline)) opd_ptr(int x) { return x * 3 + 1; }
static int __attribute__((noinline)) opd_ret(int x) { return x * 5 + 2; }
static int __attribute__((noinline)) opd_tab_a(int x) { return x * 7 + 3; }
static int __attribute__((noinline)) opd_tab_b(int x) { return x * 11 + 4; }
extern int opd_global(int);

int (*volatile opd_fp)(int) = opd_ptr;
int (*volatile opd_tab[2])(int) = { opd_tab_a, opd_tab_b };
int (*volatile opd_gp)(int) = opd_global;
volatile int opd_sel;
int (*opd_getfp(void))(int) { return opd_ret; }

int main(void)
{
    return opd_fp(1) + opd_getfp()(2) + opd_tab[opd_sel & 1](3)
           + opd_tab[(opd_sel + 1) & 1](4) + opd_gp(5);
}
EOF
cat > "$work/other.c" <<'EOF'
int __attribute__((noinline)) opd_global(int x) { return x * 13 + 5; }
EOF

cat > "$work/check.py" <<'EOF'
import struct, sys

FUNCS = ["opd_ptr", "opd_ret", "opd_tab_a", "opd_tab_b", "opd_global"]
SHF_EXECINSTR = 4

def refuse(msg):
    # A shape this checker does not model: exit 2, never the finding status.
    print(msg)
    sys.exit(2)

class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
            refuse("REFUSE: %s is not a big-endian ELF64" % path)
        shoff, = struct.unpack_from(">Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(">HHH", d, 0x3a)
        self.secs = []
        for i in range(shnum):
            f = struct.unpack_from(">IIQQQQIIQQ", d, shoff + i * shentsize)
            self.secs.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3],
                                  off=f[4], size=f[5], link=f[6], align=f[8], ent=f[9]))
        strs = self.secs[shstrndx]
        for s in self.secs:
            s["name"] = self.cstr(strs["off"] + s["name"])
        self.syms = {}
        for s in self.secs:
            if s["type"] != 2:  # SHT_SYMTAB
                continue
            names = self.secs[s["link"]]
            for i in range(s["size"] // 24):
                n, info, other, shndx, value, size = struct.unpack_from(">IBBHQQ", d, s["off"] + i * 24)
                name = self.cstr(names["off"] + n)
                if (info & 0xf) == 2 and 0 < shndx < len(self.secs):  # STT_FUNC
                    self.syms.setdefault(name, []).append((value, size, shndx))

    def cstr(self, off):
        return self.data[off:self.data.index(b"\0", off)].decode()

    def sec(self, name):
        found = [s for s in self.secs if s["name"] == name]
        return found[0] if len(found) == 1 else None

    def at(self, addr, n):
        for s in self.secs:
            if s["type"] == 1 and s["addr"] <= addr and addr + n <= s["addr"] + s["size"]:
                return s, self.data[s["off"] + addr - s["addr"]:s["off"] + addr - s["addr"] + n]
        return None, None

    def func(self, name):
        found = self.syms.get(name, [])
        if len(found) != 1:
            refuse("REFUSE: %d definitions of %s" % (len(found), name))
        return found[0]

def descriptor(elf, opd, name):
    value, size, shndx = elf.func(name)
    if elf.secs[shndx] is not opd or (value - opd["addr"]) % 8 or size == 0:
        refuse("REFUSE: %s is not an 8-byte .opd descriptor" % name)
    _, words = elf.at(value, 8)
    entry, toc = struct.unpack(">II", words)
    return value, size, entry, toc

def check(gc_path, ref_path):
    gc, ref = Elf(gc_path), Elf(ref_path)
    findings = []
    opds = {}
    for e in (gc, ref):
        opd = e.sec(".opd")
        if opd is None or opd["align"] != 4 or opd["size"] % 8:
            refuse("REFUSE: no compact .opd (4-aligned, 8-byte entries)")
        opds[id(e)] = opd
    for name in FUNCS:
        addr, size, entry, toc = descriptor(gc, opds[id(gc)], name)
        _, _, rentry, _ = descriptor(ref, opds[id(ref)], name)
        if entry == 0:
            findings.append("%s: descriptor entry word is 0" % name); continue
        if toc == 0:
            findings.append("%s: descriptor TOC word is 0" % name)
        sec, code = gc.at(entry, size)
        if sec is None or not sec["flags"] & SHF_EXECINSTR:
            findings.append("%s: entry 0x%x is not in an executable section" % (name, entry)); continue
        _, rcode = ref.at(rentry, size)
        if code != rcode:
            findings.append("%s: code at entry 0x%x differs from the non-GC link" % (name, entry))
    # main must not branch into .opd (a bl against a descriptor that GC
    # dropped stays aimed at the descriptor).
    opd = opds[id(gc)]
    _, msize, mentry, _ = descriptor(gc, opd, "main")
    _, code = gc.at(mentry, msize)
    if code is None:
        findings.append("main: entry 0x%x is not in the image" % mentry)
    else:
        for i in range(0, len(code) - 3, 4):
            w, = struct.unpack_from(">I", code, i)
            if w >> 26 != 18:
                continue
            li = w & 0x03fffffc
            if li & 0x02000000:
                li -= 0x04000000
            target = li if w & 2 else mentry + i + li
            if opd["addr"] <= target < opd["addr"] + opd["size"]:
                hit = [n for n, v in gc.syms.items() if any(x[0] == target for x in v)]
                findings.append("main+0x%x: branch into .opd at 0x%x (%s)" % (i, target, ",".join(hit) or "?"))
    for f in findings:
        print("finding: " + f)
    print("%s: %d findings" % (gc_path, len(findings)))
    return 1 if findings else 0

def doctor(src, dst, how):
    elf = Elf(src)
    data = bytearray(elf.data)
    opd = elf.sec(".opd")
    def word_off(addr):
        sec, _ = elf.at(addr, 4)
        return sec["off"] + addr - sec["addr"]
    if how == "zero":
        a, _, _, _ = descriptor(elf, opd, "opd_ptr")
        struct.pack_into(">I", data, word_off(a), 0)
    elif how == "swap":
        a, _, ea, _ = descriptor(elf, opd, "opd_tab_a")
        b, _, eb, _ = descriptor(elf, opd, "opd_tab_b")
        struct.pack_into(">I", data, word_off(a), eb)
        struct.pack_into(">I", data, word_off(b), ea)
    elif how == "bl":
        target, _, _, _ = descriptor(elf, opd, "opd_global")
        _, msize, mentry, _ = descriptor(elf, opd, "main")
        _, code = elf.at(mentry, msize)
        for i in range(0, len(code) - 3, 4):
            w, = struct.unpack_from(">I", code, i)
            if w >> 26 == 18 and w & 1 and not w & 2:
                disp = (target - (mentry + i)) & 0x03fffffc
                struct.pack_into(">I", data, word_off(mentry + i), (18 << 26) | disp | 1)
                break
        else:
            refuse("CONTROL-UNBUILDABLE: main has no bl to retarget")
    if bytes(data) == elf.data:
        refuse("CONTROL-UNBUILDABLE: %s doctoring changed nothing" % how)
    open(dst, "wb").write(bytes(data))

if sys.argv[1] == "check":
    sys.exit(check(sys.argv[2], sys.argv[3]))
doctor(sys.argv[2], sys.argv[3], sys.argv[4])
EOF

status=0
ok() { echo "opd-gc-sections: ok   $*"; }
fail() { echo "opd-gc-sections: FAIL $*"; status=1; }

for abi in ilp32 lp64; do
    flag=(); [ "$abi" = lp64 ] && flag=(-mlp64)
    built=1
    for mode in nogc gc; do
        gcflag=(); [ "$mode" = gc ] && gcflag=(-Wl,--gc-sections)
        if ! "$cc" "${B[@]}" "${flag[@]}" -O2 -ffunction-sections -fdata-sections \
                "$work/fixture.c" "$work/other.c" "${gcflag[@]}" \
                -o "$work/$abi-$mode.elf" > "$work/$abi-$mode.log" 2>&1; then
            fail "$abi $mode: link failed"; cat "$work/$abi-$mode.log"; built=0
        fi
    done
    [ $built -eq 1 ] || continue

    # Controls first: the checker must reject each doctored GC link.
    for ctl in "zero|finding: opd_ptr: descriptor entry word is 0" \
               "swap|finding: opd_tab_a: code at entry .* differs from the non-GC link" \
               "bl|finding: main+0x[0-9a-f]*: branch into .opd at 0x[0-9a-f]* (opd_global)"; do
        how="${ctl%%|*}"; want="${ctl#*|}"
        if ! python3 "$work/check.py" doctor "$work/$abi-gc.elf" "$work/$abi-$how.elf" "$how" > "$work/$abi-$how.dlog" 2>&1; then
            fail "$abi control $how: could not be built"; cat "$work/$abi-$how.dlog"; continue
        fi
        python3 "$work/check.py" check "$work/$abi-$how.elf" "$work/$abi-nogc.elf" > "$work/$abi-$how.out" 2>&1
        rc=$?
        if [ $rc -eq 1 ] && grep -q -- "^$want\$" "$work/$abi-$how.out"; then
            ok "$abi control $how: rejected"
        else
            fail "$abi control $how: exit $rc, expected 1 with '$want'"; cat "$work/$abi-$how.out"
        fi
    done

    for mode in nogc gc; do
        python3 "$work/check.py" check "$work/$abi-$mode.elf" "$work/$abi-nogc.elf" > "$work/$abi-$mode.out" 2>&1
        rc=$?
        if [ $rc -eq 0 ] && grep -q ": 0 findings$" "$work/$abi-$mode.out"; then
            ok "$abi $mode: every descriptor-only function kept with its code"
        else
            fail "$abi $mode: exit $rc"; cat "$work/$abi-$mode.out"
        fi
    done
done

[ $status -eq 0 ] && echo "opd-gc-sections: PASS" || echo "opd-gc-sections: FAIL"
exit $status

#!/usr/bin/env bash
# cgnv2elf: pack .vpo/.fpo containers into an ELF shader archive.
#
# Compiles a few small shaders with rsx-cg-compiler, converts them singly
# and as a folder, and checks the archive structure: ELF32 big-endian
# ET_REL for machine 0x528e, the fixed section order, the .note, one symbol
# per program named after its input file, .textNNNN equal to the input
# ucode, the program count, folder ordering, -s and -e, the default output
# path, and the refusals (missing input, -u, a missing output directory).
#
# usage: cgnv2elf-test.sh [rsx-cg-compiler] [cgnv2elf]
# cgnv2elf defaults to the binary beside rsx-cg-compiler.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
exe=""; [[ "$compiler" == *.exe ]] && exe=".exe"
tool="${2:-$(dirname "$compiler")/cgnv2elf$exe}"
python="$(command -v python3 || command -v python)"
fail=0
bad() { printf 'FAIL: %s\n' "$*" >&2; fail=1; }
ok() { printf 'ok   %s\n' "$*"; }
[[ -x "$compiler" ]] || { bad "rsx-cg-compiler not executable: $compiler"; exit 1; }
[[ -x "$tool" ]] || { bad "cgnv2elf not executable: $tool"; exit 1; }
[[ -n "$python" ]] || { bad "python3 not found"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/in"

# check.py <archive> [--nosem] <name>=<container> ...
# Validates the archive against its input containers, in order.
cat >"$work/check.py" <<'EOF'
import struct, sys

def fail(msg):
    print("check: " + msg)
    sys.exit(1)

args = sys.argv[1:]
elf = open(args.pop(0), "rb").read()
nosem = "--nosem" in args
progs = []
for a in args:
    if a == "--nosem":
        continue
    name, path = a.split("=", 1)
    c = open(path, "rb").read()
    profile, rev, _, _, _, _, usz, uc = struct.unpack_from(">8I", c, 0)
    progs.append((name, profile, c[uc:uc + usz]))
n = len(progs)

if elf[:9] != b"\x7fELF\x01\x02\x01\x13\x01":
    fail("ident %s" % elf[:9].hex())
(etype, mach, ver, entry, phoff, shoff, flags, ehsize, phentsize, phnum,
 shentsize, shnum, shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", elf, 16)
if (etype, mach, shoff, shentsize, shstrndx, phnum) != (1, 0x528E, 0x34, 40, 1, 0):
    fail("ELF header type %d machine %#x shoff %#x" % (etype, mach, shoff))
if shnum != 7 + 2 * n:
    fail("%d sections for %d programs" % (shnum, n))

secs = [struct.unpack_from(">10I", elf, shoff + 40 * i) for i in range(shnum)]
shs = secs[1]
strtab_sh = elf[shs[4]:shs[4] + shs[5]]
def cstr(tab, off):
    return tab[off:tab.index(b"\0", off)].decode()
names = [cstr(strtab_sh, s[0]) for s in secs]
want = ["", ".shstrtab", ".note", ".strtab", ".const", ".symtab", ".shadertab"]
for i in range(n):
    want += [".text%04d" % i, ".paramtab%04d" % i]
if names != want:
    fail("section names %s" % names)
kinds = {".shstrtab": (3, 0), ".note": (7, 2), ".strtab": (3, 2), ".const": (1, 2),
         ".symtab": (2, 2), ".shadertab": (1, 2)}
end = 0
for nm, s in zip(names[1:], secs[1:]):
    typ, fl = kinds.get(nm, (1, 6) if nm.startswith(".text") else (1, 2))
    if (s[1], s[2]) != (typ, fl):
        fail("%s type %d flags %#x" % (nm, s[1], s[2]))
    if s[8] and s[4] % s[8]:
        fail("%s offset %#x not aligned to %d" % (nm, s[4], s[8]))
    if s[4] < end:
        fail("%s overlaps the previous section" % nm)
    end = s[4] + s[5]
    if s[5]:
        last = end
if len(elf) != last:
    fail("file is %d bytes, last section ends at %d" % (len(elf), last))
sec = {nm: elf[s[4]:s[4] + s[5]] for nm, s in zip(names, secs)}

note = sec[".note"]
if note[:12] != struct.pack(">III", 12, 4, 0) or note[12:24] != b"SCE cgnv2elf" or len(note) != 28:
    fail("note %s" % note.hex())

strtab = sec[".strtab"]
sym = sec[".symtab"]
if len(sym) != 16 * (n + 1) or sym[:16] != b"\0" * 16:
    fail("symtab holds %d bytes for %d programs" % (len(sym), n))
if len(sec[".shadertab"]) != 0x1C * n:
    fail("shadertab holds %d bytes for %d programs" % (len(sec[".shadertab"]), n))
sems = 0
for i, (name, profile, ucode) in enumerate(progs):
    st_name, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", sym, 16 * (i + 1))
    if cstr(strtab, st_name) != name or value != i or info != 0x1E or shndx != 6 + 2 * i:
        fail("symbol %d is %r value %d info %#x shndx %d, want %r" %
             (i + 1, cstr(strtab, st_name), value, info, shndx, name))
    prof = struct.unpack_from(">H", sec[".shadertab"], 0x1C * i)[0]
    if prof != profile:
        fail("shadertab %d profile %d, input %d" % (i, prof, profile))
    if sec[".text%04d" % i] != ucode:
        fail(".text%04d differs from %s's ucode" % (i, name))
    pt = sec[".paramtab%04d" % i]
    count, ri, do, dc, so, sc = struct.unpack_from(">6H", pt, 0)
    if so + 8 * sc != len(pt) or do + 4 * dc != so or not (12 + 8 * count <= ri <= do):
        fail(".paramtab%04d layout count %d ri %#x def %#x sem %#x" % (i, count, ri, do, so))
    for k in range(sc):
        r, pad, off = struct.unpack_from(">HHI", pt, so + 8 * k)
        if r >= count or pad:
            fail(".paramtab%04d semantic entry %d" % (i, k))
        cstr(strtab, off)
    sems += sc
if nosem and (sems or b"TEXCOORD0\0" in strtab):
    fail("-s left %d semantic entries" % sems)
if not nosem and n and not sems:
    fail("no semantic entries without -s")
print("programs=%d" % n)
EOF
check() { "$python" "$work/check.py" "$@" >"$work/check.log" 2>&1; }

# convert <expected-exit> <args...>: runs cgnv2elf, 0 = status matched.
convert() {
    local want="$1"; shift
    "$tool" -q "$@" >"$work/tool.log" 2>&1
    local rc=$?
    [[ $rc -eq $want ]] && return 0
    printf 'exit %d, want %d: %s\n' "$rc" "$want" "$(head -1 "$work/tool.log")" >"$work/check.log"
    return 1
}

cat >"$work/frag.cg" <<'EOF'
float4 main(float2 uv : TEXCOORD0, uniform sampler2D tex, uniform float4 tint) : COLOR
{
    return tex2D(tex, uv) * tint;
}
EOF
cat >"$work/vert.cg" <<'EOF'
void main(float4 p : POSITION, float4 c : COLOR, uniform float4x4 mvp,
          out float4 op : POSITION, out float4 oc : COLOR0)
{
    op = mul(mvp, p);
    oc = c;
}
EOF
cat >"$work/plain.cg" <<'EOF'
float4 main(float4 c : COLOR0) : COLOR { return c.zyxw; }
EOF
# Mixed-case names: the folder order is case-insensitive (a, B, c), which
# byte order (B, a, c) would get wrong.
"$compiler" -p sce_fp_rsx --emit-container "$work/in/a_frag.fpo" "$work/frag.cg" >/dev/null 2>&1 \
    || bad "rsx-cg-compiler failed on the fragment shader"
"$compiler" -p sce_vp_rsx --emit-container "$work/in/B_vert.vpo" "$work/vert.cg" >/dev/null 2>&1 \
    || bad "rsx-cg-compiler failed on the vertex shader"
"$compiler" -p sce_fp_rsx --emit-container "$work/in/c.plain.fpo" "$work/plain.cg" >/dev/null 2>&1 \
    || bad "rsx-cg-compiler failed on the plain shader"
[[ $fail -eq 0 ]] || exit 1
printf 'not a shader container\n' >"$work/in/readme.txt"
A="$work/in/a_frag.fpo"; B="$work/in/B_vert.vpo"; C="$work/in/c.plain.fpo"

# Single programs.
if convert 0 "$A" "$work/a.elf" && check "$work/a.elf" "a_frag=$A"; then
    ok "fragment program: structure, symbol a_frag, .text0000 = ucode"
else bad "fragment program: $(cat "$work/check.log")"; fi
if convert 0 "$B" "$work/b.elf" && check "$work/b.elf" "B_vert=$B"; then
    ok "vertex program: structure, symbol B_vert, .text0000 = ucode"
else bad "vertex program: $(cat "$work/check.log")"; fi
if convert 0 "$C" "$work/c.bin" && check "$work/c.bin" "c.plain=$C"; then
    ok "symbol drops only the last extension (c.plain), whatever the output name"
else bad "symbol name: $(cat "$work/check.log")"; fi
if convert 0 -e "$C" "$work/ce.elf" && check "$work/ce.elf" "c.plain.fpo=$C"; then
    ok "-e keeps the extension in the symbol name"
else bad "-e: $(cat "$work/check.log")"; fi
if convert 0 -s "$A" "$work/as.elf" && check "$work/as.elf" --nosem "a_frag=$A"; then
    ok "-s drops the semantic table and strings"
else bad "-s: $(cat "$work/check.log")"; fi

# Record contents.  records.py recomputes a flat program's parameter table
# (no struct, array or matrix parameters) from its input container by the
# format rules and compares every field: record name string, type code,
# resource, flags, the fragment resource-index table, semantics, and each
# default's .const payload.  --literal also pins one record's default to
# the float4 written in the shader source, independently of the container.
cat >"$work/records.py" <<'EOF'
import struct, sys

def fail(msg):
    print("records: " + msg)
    sys.exit(1)

elf_path, container_path = sys.argv[1], sys.argv[2]
literal = None
if len(sys.argv) > 3:
    name, values = sys.argv[3].split("=", 1)
    literal = (name, struct.pack(">4f", *[float(v) for v in values.split(",")]))
elf = open(elf_path, "rb").read()
c = open(container_path, "rb").read()

def cstr(b, o):
    return b[o:b.index(b"\0", o)].decode() if o else None

profile, _, _, count, parr = struct.unpack_from(">5I", c, 0)
fp = profile == 7004
params = []
for i in range(count):
    f = struct.unpack_from(">4I8I", c, parr + 48 * i)
    ec = []
    if f[6]:
        n = struct.unpack_from(">I", c, f[6])[0]
        ec = list(struct.unpack_from(">%dI" % n, c, f[6] + 4))
    params.append(dict(type=f[0], res=f[1], var=f[2], ri=struct.unpack(">i", struct.pack(">I", f[3]))[0],
                       name=cstr(c, f[4]), dflt=c[f[5]:f[5] + 16] if f[5] else None, ec=ec,
                       sem=cstr(c, f[7]), dir=f[8], paramno=f[9], ref=f[10], shared=f[11]))
for p in params:
    if any(ch in p["name"] for ch in ".[") or p["type"] in (0x420, 0x423, 0x428):
        fail("not a flat program: %s" % p["name"])

shoff, = struct.unpack_from(">I", elf, 0x20)
shnum, = struct.unpack_from(">H", elf, 0x30)
secs = [struct.unpack_from(">10I", elf, shoff + 40 * i) for i in range(shnum)]
shs = elf[secs[1][4]:secs[1][4] + secs[1][5]]
sec = {cstr(shs, s[0]) or "": elf[s[4]:s[4] + s[5]] for s in secs}
strtab, const, pt = sec[".strtab"], sec[".const"], sec[".paramtab0000"]

# Fragment resource-index table: every leaf in order, i16 resIndex, u16 n, u16 offsets[n].
ritab, hw = b"", []
for p in params:
    hw.append(len(ritab) // 2)
    if fp:
        ritab += struct.pack(">hH", p["ri"], len(p["ec"])) + b"".join(struct.pack(">H", e) for e in p["ec"])

n, riOff, defOff, defCount, semOff, semCount = struct.unpack_from(">6H", pt, 0)
if n != len(params):
    fail("%d records for %d parameters" % (n, len(params)))
types = 12 + 8 * n
if pt[riOff:defOff] != ritab:
    fail("resource-index table %s, want %s" % (pt[riOff:defOff].hex(), ritab.hex()))
want_defs, want_sems = [], []
for i, p in enumerate(params):
    nameOff, typeOff, flags = struct.unpack_from(">IHH", pt, 12 + 8 * i)
    if cstr(strtab, nameOff) != p["name"]:
        fail("record %d name %r, want %r" % (i, cstr(strtab, nameOff), p["name"]))
    direct = p["var"] == 0x1005 or 0x429 <= p["type"] <= 0x42D
    res = (p["res"] if direct else (hw[i] if fp else p["ri"])) & 0xFFFF
    ty, rs = struct.unpack_from(">HH", pt, types + typeOff)
    if (ty, rs) != (p["type"], res):
        fail("record %d (%s) type %#x resource %#x, want %#x %#x" % (i, p["name"], ty, rs, p["type"], res))
    want = {0x1006: 1, 0x1007: 2}.get(p["var"], 0) | {0x1002: 4, 0x1003: 8}.get(p["dir"], 0)
    want |= (0x10 if p["ref"] else 0) | (0x20 if p["shared"] else 0) | 0x1000
    want |= 0x40 if p["paramno"] == 0xFFFFFFFF else 0x80 if p["paramno"] == 0xFFFFFFFE else 0
    if p["sem"] and p["sem"].startswith(("COLOR", "NORMAL")):
        want |= 0x2000
    if flags != want:
        fail("record %d (%s) flags %#x, want %#x" % (i, p["name"], flags, want))
    if p["dflt"]:
        want_defs.append((i, p["dflt"]))
    if p["sem"]:
        want_sems.append((i, p["sem"]))
if defCount != len(want_defs) or semCount != len(want_sems):
    fail("%d defaults / %d semantics, want %d / %d" % (defCount, semCount, len(want_defs), len(want_sems)))
payload = {}
for k, (i, data) in enumerate(want_defs):
    r, word = struct.unpack_from(">HH", pt, defOff + 4 * k)
    if r != i or const[4 * word:4 * word + 16] != data:
        fail("default %d: record %d word %d payload %s, want record %d payload %s" %
             (k, r, word, const[4 * word:4 * word + 16].hex(), i, data.hex()))
    payload[params[i]["name"]] = const[4 * word:4 * word + 16]
for k, (i, sem) in enumerate(want_sems):
    r, pad, off = struct.unpack_from(">HHI", pt, semOff + 8 * k)
    if (r, pad, cstr(strtab, off)) != (i, 0, sem):
        fail("semantic %d: record %d %r, want record %d %r" % (k, r, cstr(strtab, off), i, sem))
if literal and payload.get(literal[0]) != literal[1]:
    fail("%s default %s, want %s" % (literal[0], (payload.get(literal[0]) or b"").hex(), literal[1].hex()))
print("records=%d defaults=%d semantics=%d" % (n, defCount, semCount))
EOF
records() { "$python" "$work/records.py" "$@" >"$work/check.log" 2>&1; }

# A vertex literal becomes a constant parameter carrying a float4 default.
mkdir -p "$work/lit"
cat >"$work/lit.cg" <<'EOF'
void main(float4 p : POSITION, uniform float4 k, out float4 op : POSITION)
{
    op = p * k + float4(0.25, 0.5, 0.75, 1.0);
}
EOF
L="$work/lit/lit.vpo"
"$compiler" -p sce_vp_rsx --emit-container "$L" "$work/lit.cg" >/dev/null 2>&1 \
    || bad "rsx-cg-compiler failed on the literal shader"
if convert 0 "$A" "$work/a.elf" && records "$work/a.elf" "$A"; then
    ok "fragment records match the format rules ($(cat "$work/check.log"))"
else bad "fragment records: $(cat "$work/check.log")"; fi
if convert 0 "$L" "$work/lit.elf" && check "$work/lit.elf" "lit=$L" \
        && records "$work/lit.elf" "$L" "internal-constant-0=0.25,0.5,0.75,1.0"; then
    ok "vertex records match, literal default {0.25, 0.5, 0.75, 1.0} in .const"
else bad "vertex records: $(cat "$work/check.log")"; fi

# Corruption controls: one flipped byte in the paramtab record area, in its
# type table, or in the .const payload must each be caught.
# flip <archive> <out> <section> <offset-within-section>
flip() {
    "$python" - "$@" <<'EOF'
import struct, sys
src, dst, name, at = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
b = bytearray(open(src, "rb").read())
shoff, = struct.unpack_from(">I", b, 0x20)
shnum, = struct.unpack_from(">H", b, 0x30)
secs = [struct.unpack_from(">10I", b, shoff + 40 * i) for i in range(shnum)]
shs = bytes(b[secs[1][4]:secs[1][4] + secs[1][5]])
for s in secs:
    if shs[s[0]:shs.index(b"\0", s[0])].decode() == name:
        b[s[4] + at] ^= 0x01
open(dst, "wb").write(b)
EOF
}
controls=0
for spot in ".paramtab0000 0x0f" ".paramtab0000 0x2d" ".const 0x0"; do
    set -- $spot
    flip "$work/lit.elf" "$work/flip.elf" "$1" "$2"
    if cmp -s "$work/lit.elf" "$work/flip.elf"; then
        bad "corruption control $spot: byte not flipped"
    elif records "$work/flip.elf" "$L" "internal-constant-0=0.25,0.5,0.75,1.0"; then
        bad "corruption control $spot: checker accepted a flipped byte"
    else
        controls=$((controls + 1))
    fi
done
[[ $controls -eq 3 ]] && ok "record checker rejects a flipped byte in records, type table and .const"

# Input budgets: over-budget or malformed containers are refused promptly
# (1 GiB address space, 20 s) with exit 1, no output, and the tool's own
# diagnostic for that budget.  An allocation failure also exits 1 without
# output, so the text is what tells a bounded refusal from a crash.
# synth <out> <name> [name-offset]: a one-parameter fragment container.
synth() {
    "$python" - "$@" <<'EOF'
import struct, sys
out, name = sys.argv[1], sys.argv[2]
b = bytearray(32 + 48 + 22)
no = len(b)
b += name.encode() + b"\0"
if len(sys.argv) > 3:
    no = int(sys.argv[3], 0)
struct.pack_into(">12I", b, 32, 0x418, 0xcb8, 0x1006, 0xffffffff, no, 0, 0, 0, 0x1001, 0, 1, 0)
uc = len(b)
b += b"\0" * 16
struct.pack_into(">8I", b, 0, 7004, 6, len(b), 1, 32, 80, 16, uc)
open(out, "wb").write(b)
EOF
}
# alias <real-container> <out> <params> <entries>: the real container with
# its parameter array replaced by <params> copies of its first parameter,
# every copy pointing at ONE appended list of <entries> ucode offsets.
alias_ec() {
    "$python" - "$@" <<'EOF'
import struct, sys
src, out, n, entries = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
b = bytearray(open(src, "rb").read())
parr = struct.unpack_from(">I", b, 16)[0]
rec = bytearray(b[parr:parr + 48])
ec = len(b)
b += struct.pack(">I", entries) + struct.pack(">I", 0x20) * entries
struct.pack_into(">I", rec, 24, ec)
new_parr = len(b)
for _ in range(n):
    b += rec
struct.pack_into(">I", b, 12, n)
struct.pack_into(">I", b, 16, new_parr)
struct.pack_into(">I", b, 8, len(b))
open(out, "wb").write(b)
EOF
}
bounded() {
    ( ulimit -c 0; ulimit -v 1048576
      exec timeout 20 "$tool" -q "$@" ) >"$work/tool.log" 2>&1
}
synth "$work/deep.fpo" "$("$python" -c 'print(".".join(["a"] * 10000))')"
synth "$work/depth.fpo" "$("$python" -c 'print(".".join(["a"] * 500))')"
synth "$work/offset.fpo" "a" 0x7ffffff0
synth "$work/sane.fpo" "a.b"
cp "$work/sane.fpo" "$work/count.fpo"
"$python" -c 'import struct,sys; p=sys.argv[1]; b=bytearray(open(p,"rb").read()); struct.pack_into(">I",b,12,9000); open(p,"wb").write(b)' "$work/count.fpo"
alias_ec "$A" "$work/alias.fpo" 8192 65535
alias_ec "$A" "$work/alias-ok.fpo" 16 4
if convert 0 "$work/sane.fpo" "$work/sane.elf"; then
    ok "synthetic control container converts"
else bad "synthetic control container: $(cat "$work/check.log")"; fi
if convert 0 "$work/alias-ok.fpo" "$work/alias-ok.elf"; then
    ok "aliased embedded-constant control (16 parameters x 4 offsets) converts"
else bad "aliased control: $(cat "$work/check.log")"; fi
budget_rows=(
    "deep|input budget exceeded: name-length|20 KB name, 10000 components"
    "depth|input budget exceeded: name-depth|500 components"
    "count|input budget exceeded: parameter-count|9000 parameters declared"
    "alias|input budget exceeded: embedded-constants|8192 parameters sharing one 65535-offset list"
    "offset|parameter name offset is past the end of the file|name offset past the end"
)
for row in "${budget_rows[@]}"; do
    IFS='|' read -r kind want what <<<"$row"
    rm -f "$work/$kind.elf"
    bounded "$work/$kind.fpo" "$work/$kind.elf"; rc=$?
    log="$(cat "$work/tool.log")"
    if [[ $rc -eq 1 && ! -e "$work/$kind.elf" && "$log" == *"$want"* \
          && "$log" != *bad_alloc* && "$log" != *terminate* ]]; then
        ok "$what: exit 1, no output, '$want'"
    else
        bad "$what: exit $rc, output $( [[ -e "$work/$kind.elf" ]] && echo written || echo absent), log: $(head -1 "$work/tool.log")"
    fi
done

# Folder: case-insensitive order, readme.txt skipped.
if convert 0 "$work/in" "$work/all.elf" \
        && check "$work/all.elf" "a_frag=$A" "B_vert=$B" "c.plain=$C"; then
    ok "folder: 3 programs in case-insensitive order, non-container skipped"
else bad "folder: $(cat "$work/check.log")"; fi
if convert 0 -s -e "$work/in" "$work/all-se.elf" && check "$work/all-se.elf" --nosem \
        "a_frag.fpo=$A" "B_vert.vpo=$B" "c.plain.fpo=$C"; then
    ok "folder with -s -e"
else bad "folder -s -e: $(cat "$work/check.log")"; fi

# Default output: <dir of input>/out<ext of input>.
mkdir -p "$work/dflt"; cp "$B" "$work/dflt/v.vpo"
if convert 0 "$work/dflt/v.vpo" && [[ -f "$work/dflt/out.vpo" ]] \
        && check "$work/dflt/out.vpo" "v=$B"; then
    ok "default output is out.vpo beside the input"
else bad "default output: $(ls "$work/dflt" | tr '\n' ' ') $(cat "$work/check.log" 2>/dev/null)"; fi

# Refusals: exit 1 and no output (an existing output left untouched).
printf 'keep\n' >"$work/keep.elf"
if convert 1 "$work/in/missing.fpo" "$work/keep.elf" && [[ "$(cat "$work/keep.elf")" == keep ]] \
        && convert 1 "$work/in/missing.fpo" "$work/none.elf" && [[ ! -e "$work/none.elf" ]]; then
    ok "missing input: exit 1, no output, existing output untouched"
else bad "missing input: $(cat "$work/check.log")"; fi
if convert 1 -u "$A" "$work/u.elf" && [[ ! -e "$work/u.elf" ]] \
        && convert 1 --no-unref "$A" "$work/u.elf" && [[ ! -e "$work/u.elf" ]]; then
    ok "-u/--no-unref refused: exit 1, no output"
else bad "-u: $(cat "$work/check.log")"; fi
if convert 1 "$A" "$work/no-such-dir/x.elf" && [[ ! -e "$work/no-such-dir" ]]; then
    ok "missing output directory: exit 1"
else bad "missing output directory: $(cat "$work/check.log")"; fi
# Not a container: the run fails but an empty archive is still written.
if convert 1 "$work/in/readme.txt" "$work/empty.elf" && check "$work/empty.elf" \
        && [[ "$(cat "$work/check.log")" == programs=0 ]]; then
    ok "non-container input: exit 1, empty archive"
else bad "non-container input: $(cat "$work/check.log")"; fi

# Atomic writes leave no temporaries behind.
leftovers="$(find "$work" -name '*.tmp-*' | head -3)"
if [[ -z "$leftovers" ]]; then ok "no temporary files left behind"
else bad "temporary files left behind: $leftovers"; fi

if [[ $fail -ne 0 ]]; then
    echo "cgnv2elf-test: FAILED" >&2
    exit 1
fi
echo "cgnv2elf-test: all checks passed"

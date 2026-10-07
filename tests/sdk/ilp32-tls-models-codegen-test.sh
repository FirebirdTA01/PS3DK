#!/usr/bin/env bash
# Codegen guard for GCC patch 0057: every PPU TLS access model compiles to
# the right relocations, or is refused with a clean diagnostic, in both data
# models.  Before 0057 the ILP32 (default) compiler hit "unrecognizable insn"
# / an ICE in extract_insn on initial-exec, the model an extern __thread
# variable gets in non-PIC code (and what global- and local-dynamic become
# without -fPIC); also on -fPIC local-dynamic and -mtls-size=16 local-exec.
#
# Each row compiles a small TU with -c and reads `objdump -dr`:
#   IE    R_PPC64_GOT_TPREL16_HA/_LO_DS + R_PPC64_TLS (add with r13)
#   GD    R_PPC64_GOT_TLSGD16_HA/_LO + R_PPC64_TLSGD on the __tls_get_addr call
#   LD    R_PPC64_GOT_TLSLD16_HA/_LO + R_PPC64_TLSLD + R_PPC64_DTPREL16_HA/_LO
#   LD16  R_PPC64_GOT_TLSLD16_HA/_LO + R_PPC64_DTPREL16
#   LD64  R_PPC64_GOT_TLSLD16_HA/_LO + R_PPC64_GOT_DTPREL16_HA/_LO_DS
#   LE    R_PPC64_TPREL16_HA/_LO, the addis based on r13
#   LE16  R_PPC64_TPREL16, the addi based on r13
#   SORRY exit 1 with "sorry, unimplemented: '-mtls-size=64' local-dynamic"
#         (ILP32 only: the DTPREL64 GOT slot has no 32-bit load)
# In every row, a GOT TP- or DTP-offset slot is 8 bytes (TPREL64/DTPREL64),
# so the instruction carrying its _LO_DS/_DS relocation must be ld: an lwz
# reads the high word whenever the linker keeps the GOT load (the silent
# wrong-code shape a Pmode-only fix would produce under ILP32).  Any
# "internal compiler error" or "unrecognizable insn" fails the row.
#
# With a driver that can link (an installed SDK), the IE, GD and LD objects
# are also linked into executables, IE both relaxed (default) and with
# --no-tls-optimize; GD/LD must relax away the __tls_get_addr call.
#
# Controls (embedded below) always run: the IE shape with lwz must be
# reported, as must a missing @tls marker and a non-r13 local-exec base; the
# patched shapes must be accepted.  Compiles skip without PS3DEV (CI has no
# PPU compiler); run it in the release gate.
# usage: ilp32-tls-models-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
#   TLS_NO_LINK=1 skips the link rows.
set -u
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
T=ilp32-tls-models-codegen

cat > "$work/scan.py" <<'EOF'
import re, sys

INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*([a-z][a-z0-9.+-]*)\s*(.*?)\s*$")
RELOC = re.compile(r"^\s*[0-9a-f]+:\s+(R_PPC64_\w+)\s+(\S+)")

def parse(path):
    insns, relocs = [], []
    for line in open(path, errors="replace"):
        m = INSN.match(line)
        if m:
            args = m.group(2).replace(" ", "")
            insns.append((m.group(1), re.sub("(?<![A-Za-z_.])r([0-9]+)", lambda r: r.group(1), args)))
            continue
        m = RELOC.match(line)
        if m and insns:
            relocs.append((m.group(1)[len("R_PPC64_"):], m.group(2), len(insns) - 1))
    return insns, relocs

REQUIRED = {
    "IE":   [("GOT_TPREL16_HA", "GOT_TPREL16_DS"), ("GOT_TPREL16_LO_DS", "GOT_TPREL16_DS"), ("TLS",)],
    "GD":   [("GOT_TLSGD16_HA", "GOT_TLSGD16"), ("GOT_TLSGD16_LO", "GOT_TLSGD16"), ("TLSGD",)],
    "LD":   [("GOT_TLSLD16_HA", "GOT_TLSLD16"), ("GOT_TLSLD16_LO", "GOT_TLSLD16"), ("TLSLD",),
             ("DTPREL16_HA",), ("DTPREL16_LO",)],
    "LD16": [("GOT_TLSLD16_HA", "GOT_TLSLD16"), ("TLSLD",), ("DTPREL16",)],
    "LD64": [("GOT_TLSLD16_HA", "GOT_TLSLD16"), ("TLSLD",),
             ("GOT_DTPREL16_HA", "GOT_DTPREL16_DS"), ("GOT_DTPREL16_LO_DS", "GOT_DTPREL16_DS")],
    "LE":   [("TPREL16_HA",), ("TPREL16_LO",)],
    "LE16": [("TPREL16",)],
}

def judge(path, cls):
    insns, relocs = parse(path)
    kinds = {r[0] for r in relocs}
    out = []
    for alts in REQUIRED[cls]:
        if not any(a in kinds for a in alts):
            out.append("missing R_PPC64_%s" % alts[0])
    for kind, sym, i in relocs:
        op, args = insns[i]
        f = args.split(",")
        if kind in ("GOT_TPREL16_LO_DS", "GOT_TPREL16_DS", "GOT_DTPREL16_LO_DS", "GOT_DTPREL16_DS") \
                and op != "ld":
            out.append("8-byte GOT slot of %s (%s) loaded by %s" % (sym, kind, op))
        if kind == "TLS" and not (op == "add" and "13" in f[1:]):
            out.append("@tls marker of %s on %s %s, expected add with r13" % (sym, op, args))
        if kind in ("TPREL16_HA", "TPREL16") and not (len(f) > 1 and f[1] == "13"):
            out.append("local-exec %s of %s based on r%s, expected r13" % (kind, sym, f[1] if len(f) > 1 else "?"))
    if cls in ("GD", "LD", "LD16", "LD64"):
        if not any(k == "REL24" and s.lstrip(".").startswith("__tls_get_addr") for k, s, _ in relocs):
            out.append("no call to __tls_get_addr")
    return out

path, cls = sys.argv[1], sys.argv[2]
findings = judge(path, cls)
for f in findings:
    print("finding: %s" % f)
print("%s: %s, %d findings" % (path, cls, len(findings)))
sys.exit(0 if not findings else 3)
EOF

# --- controls -----------------------------------------------------------
# The patched ILP32 initial-exec shape (medium code model).
cat > "$work/ie-green.d" <<'EOF'
0000000000000000 <.f>:
   0:	3d 22 00 00 	addis   r9,r2,0
			2: R_PPC64_GOT_TPREL16_HA	x
   4:	e9 29 00 00 	ld      r9,0(r9)
			6: R_PPC64_GOT_TPREL16_LO_DS	x
   8:	7d 29 6a 14 	add     r9,r9,r13
			8: R_PPC64_TLS	x
   c:	80 69 00 00 	lwz     r3,0(r9)
  10:	4e 80 00 20 	blr
EOF
# A Pmode-only fix: lwz reads the high word of the 8-byte TPREL64 slot.
sed 's/e9 29 00 00 \tld      r9,0(r9)/81 29 00 00 \tlwz     r9,0(r9)/' "$work/ie-green.d" > "$work/ie-lwz.d"
# The @tls marker on something other than an add of r13.
sed 's/7d 29 6a 14 \tadd     r9,r9,r13/7d 29 52 14 \tadd     r9,r9,r10/' "$work/ie-green.d" > "$work/ie-nor13.d"
# The relocations of a local-exec sequence where an initial-exec one belongs.
cat > "$work/le-green.d" <<'EOF'
0000000000000000 <.f>:
   0:	3d 2d 00 00 	addis   r9,r13,0
			2: R_PPC64_TPREL16_HA	x
   4:	39 29 00 00 	addi    r9,r9,0
			6: R_PPC64_TPREL16_LO	x
   8:	80 69 00 00 	lwz     r3,0(r9)
  10:	4e 80 00 20 	blr
EOF
# Patch 0028's wrong-code shape: local-exec based on the TOC pointer.
sed 's/3d 2d 00 00 \taddis   r9,r13,0/3d 22 00 00 \taddis   r9,r2,0/' "$work/le-green.d" > "$work/le-r2.d"

status=0
control() {  # <file> <class> <expected exit> <expected text> <label>
    python3 "$work/scan.py" "$work/$1" "$2" > "$work/$1.out"
    local rc=$?
    if [ $rc -eq "$3" ] && grep -q -- "$4" "$work/$1.out"; then
        echo "$T: ok   control: $5"
    else
        echo "$T: FAIL control: $5 (exit $rc)"; cat "$work/$1.out"; status=1
    fi
}
control ie-green.d IE 0 "0 findings" "the patched initial-exec shape is accepted"
control ie-lwz.d IE 3 "8-byte GOT slot of x (GOT_TPREL16_LO_DS) loaded by lwz" "an lwz of the TPREL64 slot is reported"
control ie-nor13.d IE 3 "@tls marker of x on add" "an @tls add without r13 is reported"
control le-green.d IE 3 "missing R_PPC64_GOT_TPREL16_HA" "local-exec relocations do not satisfy an initial-exec row"
control le-green.d LE 0 "0 findings" "the local-exec shape is accepted"
control le-r2.d LE 3 "based on r2, expected r13" "a local-exec sequence based on r2 is reported"

ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "$T: SKIP compiles (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "$T: PASS (controls only)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "$T: FAIL: no compiler at $cc"; exit 1; }

# --- sources ------------------------------------------------------------
cat > "$work/ext.c" <<'EOF'
extern __thread int x;
int f (void) { return x; }
void g (int v) { x = v; }
int *h (void) { return &x; }
EOF
cat > "$work/attr.c" <<'EOF'
extern __thread int x __attribute__ ((tls_model ("initial-exec")));
int f (void) { return x; }
int *h (void) { return &x; }
EOF
cat > "$work/def.c" <<'EOF'
__thread int x = 3;
static __thread int y;
int f (void) { return x + y; }
int *h (void) { return &x; }
int *k (void) { return &y; }
EOF
cat > "$work/xdef.c" <<'EOF'
__thread int x = 3;
EOF
cat > "$work/main.c" <<'EOF'
extern int f (void);
extern int *h (void);
int main (void) { return f () + (h () != 0); }
EOF

# name|source|flags|ILP32 class|LP64 class
rows='ie-extern|ext.c||IE|IE
ie-flag|ext.c|-fPIC -ftls-model=initial-exec|IE|IE
ie-attr|attr.c|-fPIC|IE|IE
ie-gd-nopic|ext.c|-ftls-model=global-dynamic|IE|IE
ie-ld-nopic|ext.c|-ftls-model=local-dynamic|IE|IE
gd|ext.c|-fPIC|GD|GD
ld|def.c|-fPIC -ftls-model=local-dynamic|LD|LD
ld16|def.c|-fPIC -ftls-model=local-dynamic -mtls-size=16|LD16|LD16
ld64|def.c|-fPIC -ftls-model=local-dynamic -mtls-size=64|SORRY|LD64
le|def.c||LE|LE
le16|def.c|-mtls-size=16|LE16|LE16
le64|def.c|-mtls-size=64|IE|IE'

while IFS='|' read -r name src flags c32 c64; do
  for model in ilp32 lp64; do
    abi=""; cls=$c32; [ $model = lp64 ] && { abi=-mlp64; cls=$c64; }
    for opt in -O0 -O2; do
      tag="$model $opt $name"
      o="$work/$model$opt-$name.o"
      "$cc" ${EXTRA_CFLAGS:-} $abi $opt $flags -c "$work/$src" -o "$o" > "$o.err" 2>&1
      rc=$?
      if grep -q -E "internal compiler error|unrecognizable insn|Segmentation fault" "$o.err"; then
          echo "$T: FAIL $tag: $(grep -m1 -E 'internal compiler error|unrecognizable insn|Segmentation' "$o.err" | tr -d '\r')"
          status=1; continue
      fi
      if [ "$cls" = SORRY ]; then
          if [ $rc -eq 1 ] && grep -q "sorry, unimplemented: .-mtls-size=64. local-dynamic TLS with 32-bit pointers" "$o.err"; then
              echo "$T: ok   $tag: refused (sorry)"
          else
              echo "$T: FAIL $tag: expected the -mtls-size=64 local-dynamic sorry, exit $rc"; cat "$o.err"; status=1
          fi
          continue
      fi
      if [ $rc -ne 0 ]; then
          echo "$T: FAIL $tag: compile exit $rc"; cat "$o.err"; status=1; continue
      fi
      "$objdump" -dr "$o" > "$o.d" || { echo "$T: FAIL $tag: objdump"; status=1; continue; }
      if python3 "$work/scan.py" "$o.d" "$cls" > "$o.out"; then
          echo "$T: ok   $tag: $cls ($(grep -o 'R_PPC64_[A-Z0-9_]*' "$o.d" | grep -v -E 'ADDR|REL32|REL24|TOC' | sort -u | tr '\n' ' '))"
      else
          echo "$T: FAIL $tag: expected $cls"; cat "$o.out"; status=1
      fi
    done
  done
done <<< "$rows"

# --- link rows ----------------------------------------------------------
if [ "${TLS_NO_LINK:-}" = 1 ]; then
    echo "$T: SKIP link rows (TLS_NO_LINK=1)"
elif ! "$cc" ${EXTRA_CFLAGS:-} -O2 "$work/main.c" "$work/def.c" -o "$work/probe.elf" > "$work/probe.err" 2>&1; then
    echo "$T: SKIP link rows (the driver cannot link a plain program here)"
else
  for model in ilp32 lp64; do
    abi=""; [ $model = lp64 ] && abi=-mlp64
    # x defined in its own TU; the access TU per row.
    "$cc" ${EXTRA_CFLAGS:-} $abi -O2 -c "$work/xdef.c" -o "$work/$model-xdef.o" || { status=1; continue; }
    for row in "ie|ext.c||" "ie-notlsopt|ext.c||-Wl,--no-tls-optimize" "gd|ext.c|-fPIC|" "ld|def.c|-fPIC -ftls-model=local-dynamic|"; do
      IFS='|' read -r name src flags lflags <<< "$row"
      tag="$model link $name"
      objs="$work/$model-link-$name.o"
      "$cc" ${EXTRA_CFLAGS:-} $abi -O2 $flags -c "$work/$src" -o "$objs" || { echo "$T: FAIL $tag: compile"; status=1; continue; }
      [ "$src" = ext.c ] && objs="$objs $work/$model-xdef.o"
      if "$cc" ${EXTRA_CFLAGS:-} $abi -O2 "$work/main.c" $objs $lflags -o "$work/$model-$name.elf" > "$work/$model-$name.lerr" 2>&1; then
          if "$objdump" -d "$work/$model-$name.elf" | grep -q "__tls_get_addr"; then
              echo "$T: FAIL $tag: a __tls_get_addr call survived the link"; status=1
          else
              echo "$T: ok   $tag: links"
          fi
      else
          echo "$T: FAIL $tag: link failed"; cat "$work/$model-$name.lerr"; status=1
      fi
    done
  done
fi

[ $status -eq 0 ] && echo "$T: PASS" || echo "$T: FAIL"
exit $status

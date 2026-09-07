#!/usr/bin/env bash
# fp-sources-test.sh - the shared source decoder, and the table it rests on
# (t_c83277c9).
#
# fp_sources.py names the SOURCES of a fragment instruction, which
# ucode_decode.py deliberately never did: it counts them.  Naming one needs
# two things that a field read does not, and this file is where both are held
# to account.
#
#   THE ARITY TABLE.  Every instruction carries three source words whatever it
#   reads, and the unused ones encode as TEMP R0 with the identity swizzle -
#   byte-identical to a genuine read of R0.  So the decoder cannot know how
#   many of the three to believe without an opcode-to-arity table, and that
#   table is a second source of truth about the ISA.  It is checked here
#   against every fragment container this tree can produce, in both
#   directions, and the check is itself checked by breaking the table.
#
#   THE INPUT SELECTOR.  An input source's register is hw[0] bits 13..16 -
#   ONE selector for the whole instruction - and the slot's own register field
#   stays zero.  A decoder that reports the slot field calls every varying
#   "R0"; the rows below pin the real varying.
#
# NEGATIVE CONTROLS, and they are two different claims.  Doctoring a VALID
# field proves the render is SENSITIVE to the bytes.  It says nothing about
# malformed input, so truncation and an out-of-range offset are asserted
# separately, and asserted to be REFUSALS WITH A REASON - a decoder that dies
# with a traceback cannot be told apart from one with a bug.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# A refusal is exit 1 EXACTLY; 124 is a timeout and >= 128 is a signal, and
# either would satisfy "did not exit 0" while meaning the compiler never
# reached the decision (t_fd95d1b9).
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-fp-sources-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$here/fp_sources.py"
[[ -f "$decoder" ]] || fail "fp_sources.py is missing: $decoder"

emit() {   # <stem> <source text>  -> $work/<stem>.fpo
    printf '%s\n' "$2" >"$work/$1.cg"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$1.fpo" "$work/$1.cg"
    ) >"$work/$1.log" 2>"$work/$1.err" || rc=$?
    refusal_status "$rc" "$1"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$1.err" >&2; fail "$1 did not compile"; }
    [[ -s "$work/$1.fpo" ]] || fail "$1 compiled but wrote no container"
}

srcs() { python3 "$decoder" "$work/$1.fpo"; }

want_line() {   # <stem> <extended-regex> <what it means>
    srcs "$1" >"$work/$1.srcs"
    grep -qE "$2" "$work/$1.srcs" \
        || { cat "$work/$1.srcs" >&2; fail "$1: no instruction matching '$2' - $3"; }
}

# ---- the input selector ----------------------------------------------------
# Four shaders differing only in the varying they read.  If the decoder took
# the register from the SOURCE WORD these would all render the same, because
# that field is zero for every input.
emit tex0 'float4 main(float4 a : TEXCOORD0) : COLOR { return a; }'
emit tex1 'float4 main(float4 a : TEXCOORD1) : COLOR { return a; }'
emit tex3 'float4 main(float4 a : TEXCOORD3) : COLOR { return a; }'
emit col0 'float4 main(float4 a : COLOR0) : COLOR { return a; }'
want_line tex0 '^[0-9]+ MOV .* s0=TEX0\.'  "the varying is named from hw[0]'s selector"
want_line tex1 '^[0-9]+ MOV .* s0=TEX1\.'  "TEXCOORD1 must not render as TEXCOORD0"
want_line tex3 '^[0-9]+ MOV .* s0=TEX3\.'  "TEXCOORD3 must not render as TEXCOORD0"
want_line col0 '^[0-9]+ MOV .* s0=COL0\.'  "COLOR0 must not render as a texcoord"
for pair in "tex0 tex1" "tex0 col0"; do
    set -- $pair
    cmp -s "$work/$1.srcs" "$work/$2.srcs" \
        && fail "$1 and $2 render identically - the input selector is not being read"
done
printf '  %-42s %s\n' "input selector" "TEX0/TEX1/TEX3/COL0 all distinct"

# ---- arity: a one-source op must not report three ---------------------------
emit onesrc 'float4 main(float4 a : TEXCOORD0) : COLOR { return a; }'
srcs onesrc >"$work/onesrc.srcs"
[[ "$(grep -c 's1=' "$work/onesrc.srcs" || true)" == "0" ]] \
    || { cat "$work/onesrc.srcs" >&2; fail "a MOV reported a second source; its slots 1 and 2 are padding that decodes as TEMP R0"; }
emit threesrc 'float4 main(float4 a : TEXCOORD0, uniform float k) : COLOR { return a * k + a; }'
want_line threesrc '^[0-9]+ MAD .* s0=.* s1=.* s2=' "a MAD reads all three slots"
printf '  %-42s %s\n' "arity" "MOV names one source, MAD names three"

# ---- the half destination ---------------------------------------------------
emit halfret  'half4 main() : COLOR { return 1.0; }'
emit floatret 'float4 main() : COLOR { return 1.0; }'
want_line halfret  '^[0-9]+ MOV dst=H0 '  "a declared half colour output writes H0"
want_line floatret '^[0-9]+ MOV dst=R0 '  "a float colour output writes R0"
printf '  %-42s %s\n' "destination precision" "half renders H0, float renders R0"

# ---- sensitivity: doctoring a valid field must change the render ------------
# This proves the render depends on the bytes.  It proves NOTHING about
# malformed input, which is asserted separately below.
python3 - "$work/tex0.fpo" "$work/doctored.fpo" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
hdr = struct.unpack_from('>8I', b, 0)
uc = hdr[7]
# hw[0] of the first instruction, in on-disk halfword-swapped form: flip the
# input selector from TC0 (4) to TC1 (5).  bits 13..16 of the LOGICAL word are
# bits 29..32 of the stored one, so edit the logical value and swap back.
w = struct.unpack_from('>I', b, uc)[0]
logical = ((w >> 16) | ((w & 0xFFFF) << 16)) & 0xFFFFFFFF
logical = (logical & ~(0xF << 13)) | (5 << 13)
struct.pack_into('>I', b, uc, ((logical >> 16) | ((logical & 0xFFFF) << 16)) & 0xFFFFFFFF)
open(sys.argv[2], 'wb').write(bytes(b))
PY
python3 "$decoder" "$work/doctored.fpo" >"$work/doctored.srcs"
cmp -s "$work/tex0.srcs" "$work/doctored.srcs" \
    && fail "doctoring the input selector did not change the render - the decoder is not reading the bytes"
grep -qE 's0=TEX1\.' "$work/doctored.srcs" \
    || { cat "$work/doctored.srcs" >&2; fail "the doctored selector should render as TEX1"; }
printf '  %-42s %s\n' "sensitivity" "one doctored field moves the render"

# ---- the source modifiers: HALF and ABS ------------------------------------
# A source read at half precision names an H register - H0 and H1 are the two
# halves of R0 - and |x| changes the value the instruction computes.  A render
# that drops either is not terse, it is WRONG about what the program does, and
# a valid MOV, the same MOV with its source's half bit set, and the same with
# its abs bit set must not render alike.  ABS is per-slot and in a DIFFERENT
# WORD for each source (SRC0 hw[1] bit 29, SRC1 hw[2] bit 18, SRC2 hw[3] bit
# 18), so one position cannot stand for all three.
modifier() {   # <name> <word index> <bit>
    python3 - "$work/threesrc.fpo" "$work/$1.fpo" "$2" "$3" <<'PY'
import struct, sys
src, dst, word, bit = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
b = bytearray(open(src, 'rb').read())
hdr = struct.unpack_from('>8I', b, 0)
uc, size = hdr[7], hdr[6]


def swap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


# The first instruction whose slot-0 source is a TEMP; that is the one the
# modifier bits below are meaningful on.
off = uc
while off + 16 <= uc + size:
    w = [swap(struct.unpack_from('>I', b, off + 4 * j)[0]) for j in range(4)]
    if (w[1] & 3) == 0 and ((w[0] >> 24) & 0x3F) in (0x01, 0x02, 0x03, 0x04):
        at = off + 4 * word
        v = swap(struct.unpack_from('>I', b, at)[0]) | (1 << bit)
        struct.pack_into('>I', b, at, swap(v))
        open(dst, 'wb').write(bytes(b))
        sys.exit(0)
    off += 16
    if any((w[k] & 3) == 2 for k in (1, 2, 3)):
        off += 16
sys.exit("no instruction with a temp source-0 in the fixture")
PY
    python3 "$decoder" "$work/$1.fpo" >"$work/$1.srcs"
}
srcs threesrc >"$work/threesrc.srcs"
modifier src_half 1 8
cmp -s "$work/threesrc.srcs" "$work/src_half.srcs" \
    && fail "setting a source's HALF bit did not change the render - a half source names an H register, not the R register of the same number"
grep -qE 's0=-?\|?H[0-9]' "$work/src_half.srcs" \
    || { cat "$work/src_half.srcs" >&2; fail "a half source must render as an H register"; }
modifier src_abs 1 29
cmp -s "$work/threesrc.srcs" "$work/src_abs.srcs" \
    && fail "setting SRC0_ABS (hw[1] bit 29) did not change the render - |x| changes the value the instruction computes"
grep -qE 's0=\|' "$work/src_abs.srcs" \
    || { cat "$work/src_abs.srcs" >&2; fail "an abs source must render with the bars"; }
printf '  %-42s %s\n' "source modifiers" "half names H, abs renders |x|, each moves the render"

# ---- malformed input is a REFUSAL, not a traceback --------------------------
malformed() {   # <name> <expected message fragment>
    local rc=0
    python3 "$decoder" "$work/$1.fpo" >"$work/$1.out" 2>"$work/$1.err" || rc=$?
    [[ "$rc" -eq 2 ]] || { cat "$work/$1.err" >&2; fail "$1: exited $rc, expected 2 (a named refusal)"; }
    grep -q "Traceback" "$work/$1.err" \
        && { cat "$work/$1.err" >&2; fail "$1: died with a traceback; a malformed container must be refused with a reason"; }
    grep -qE "$2" "$work/$1.err" \
        || { cat "$work/$1.err" >&2; fail "$1: refused without naming '$2'"; }
}
head -c 8 "$work/tex0.fpo" >"$work/trunc_hdr.fpo"
malformed trunc_hdr "shorter than a container header"
head -c 64 "$work/tex0.fpo" >"$work/trunc_body.fpo"
malformed trunc_body "header says"
python3 - "$work/tex0.fpo" "$work/bad_progoff.fpo" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
struct.pack_into('>I', b, 20, 0xFFFF0000)      # programOffset, word 5
open(sys.argv[2], 'wb').write(bytes(b))
PY
malformed bad_progoff "programOffset"
python3 - "$work/tex0.fpo" "$work/bad_ucode.fpo" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
struct.pack_into('>I', b, 24, 0xFFFF0000)      # ucodeSize, word 6
open(sys.argv[2], 'wb').write(bytes(b))
PY
malformed bad_ucode "ucode runs past the end"
poke() {   # <name> <byte offset> <u32 value>
    python3 - "$work/tex0.fpo" "$work/$1.fpo" "$2" "$3" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
struct.pack_into('>I', b, int(sys.argv[3]), int(sys.argv[4], 0))
open(sys.argv[2], 'wb').write(bytes(b))
PY
}
# A pointer that is merely IN RANGE is not a valid locator: programOffset 0
# puts the subtype block on top of the header, and len-1 leaves 21 of its 22
# bytes off the end.  Both used to be accepted, and the ucode still decoded,
# because the ucode is found by its own offset - so the container read clean
# while its metadata pointed at nothing (codex).
poke prog_zero 20 0
malformed prog_zero "inside the parameter table"
poke prog_end 20 "$(( $(wc -c < "$work/tex0.fpo") - 1 ))"
malformed prog_end "runs past the end"
poke ucode_in_subtype 28 0
malformed ucode_in_subtype "starts inside the program subtype block"
poke bad_profile 0 0xdeadbeef
malformed bad_profile "is not sce_fp_rsx"
poke bad_revision 4 99
malformed bad_revision "format revision"
# Word 4 is where the parameter table BEGINS.  Reading it as an end lets
# programOffset land in the MIDDLE of the table - the records are still there,
# the ucode is still found by its own offset, and the container reads clean
# while its metadata points into someone else's bytes.  And an unbounded
# parameter count puts the table's end past any real file (codex).
poke prog_in_table 20 32
malformed prog_in_table "inside the parameter table"
poke huge_nparams 12 0xffffffff
malformed huge_nparams "parameters starting at"
# And the table cannot start inside the eight-word header either.
poke table_in_header 16 8
malformed table_in_header "inside the .*-byte header"
printf '  %-42s %s\n' "malformed input" "12 shapes refused with a reason, none a traceback"

# ---- an instruction that writes NOTHING must not render a destination ------
# FENCBR sets OUT_NONE and then puts register 0x3F and mask 0xF into fields
# that are meaningless with it - deliberately, to match the reference byte for
# byte.  Rendering those as a write to R63.xyzw invents a destination exactly
# as naming an unused slot invents a read (codex).
# The BIT decides this, not the opcode name: setting OUT_NONE on an
# ordinary MOV must take its destination away too.  Every FENCBR the tree
# emits is checked further down, where the corpus containers already exist.
python3 - "$work/tex0.fpo" "$work/out_none.fpo" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
hdr = struct.unpack_from('>8I', b, 0)
uc, size = hdr[7], hdr[6]


def swap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


off = uc
while off + 16 <= uc + size:
    w = [swap(struct.unpack_from('>I', b, off + 4 * j)[0]) for j in range(4)]
    if ((w[0] >> 24) & 0x3F) == 0x01 and not ((w[0] >> 30) & 1):
        struct.pack_into('>I', b, off, swap(w[0] | (1 << 30)))
        open(sys.argv[2], 'wb').write(bytes(b))
        sys.exit(0)
    off += 16
    if any((w[k] & 3) == 2 for k in (1, 2, 3)):
        off += 16
sys.exit("no MOV with a destination in the fixture")
PY
python3 "$decoder" "$work/out_none.fpo" >"$work/out_none.srcs"
grep -qE '^[0-9]+ MOV dst=none ' "$work/out_none.srcs" \
    || { cat "$work/out_none.srcs" >&2; fail "setting OUT_NONE on a MOV did not remove its destination from the render"; }
printf '  %-42s %s\n' "OUT_NONE" "no destination is rendered, and the bit alone decides it"

# ---- the arity table, against every fragment container this tree emits ------
count=0
for src in "$shaders"/*.cg "$shaders"/*.fcg; do
    [[ -e "$src" ]] || continue
    case "$(basename "$src")" in *_v.cg|*.vcg) continue ;; esac
    out="$work/corpus_$(basename "$src").fpo"
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >/dev/null 2>&1 || rc=$?
    # A refusal here is not this test's business - the corpus contains
    # deliberate refusals - but a CRASH is nobody's idea of one.
    refusal_status "$rc" "$(basename "$src")"
    [[ "$rc" -eq 0 ]] || rm -f "$out"
    [[ -e "$out" ]] && count=$((count + 1))
done
(( count > 50 )) || fail "only $count fragment containers compiled - the corpus walk broke, so the table below was checked against almost nothing"

# Every FENCBR this tree emits - 189 of them across the corpus at the time
# of writing, about one per program - sets OUT_NONE and then puts register
# 0x3F and mask 0xF into fields that are meaningless with it.  Not one may
# render a destination.
fencbr_seen=0
for c in "$work"/corpus_*.fpo; do
    python3 "$decoder" "$c" >"$work/one.srcs"
    n=$(grep -cE '^[0-9]+ FENCBR ' "$work/one.srcs" || true)
    fencbr_seen=$((fencbr_seen + n))
    if grep -E '^[0-9]+ FENCBR ' "$work/one.srcs" | grep -qv 'dst=none'; then
        grep -E '^[0-9]+ FENCBR ' "$work/one.srcs" >&2
        fail "$(basename "$c"): a FENCBR rendered a destination; OUT_NONE means it writes nothing"
    fi
done
(( fencbr_seen > 0 )) || fail "no FENCBR was seen in $count containers - the check above examined nothing"
printf '  %-42s %s\n' "OUT_NONE" "$fencbr_seen FENCBR, none rendering a destination"

python3 "$decoder" --verify-arity "$work"/corpus_*.fpo >"$work/arity.txt" 2>&1 \
    || { cat "$work/arity.txt" >&2; fail "the arity table is CONTRADICTED by an emitted instruction"; }
grep -q "^CONTRADICTED" "$work/arity.txt" \
    && { cat "$work/arity.txt" >&2; fail "the arity table is contradicted"; }
grep -q "^UNREADABLE" "$work/arity.txt" \
    && { cat "$work/arity.txt" >&2; fail "a container the compiler just wrote could not be walked"; }
grep -q "^UNWITNESSED" "$work/arity.txt" \
    && { cat "$work/arity.txt" >&2; fail "an opcode declares a source that no emission in this corpus ever used.
That is not proof the entry is wrong - a genuine read of R0 is
byte-identical to padding - but it is an entry with no evidence behind it
and it must be measured or removed rather than left declared."; }
printf '  %-42s %s\n' "arity vs $count containers" "$(sed -n 's/^opcodes seen: //p' "$work/arity.txt" | tr ',' '\n' | wc -l) opcodes, 0 contradicted, 0 unwitnessed"

# ---- and the check itself, seen accusing -----------------------------------
# A verifier nobody has watched refuse is not a verifier.  Break the table two
# ways and require each break to be reported.
python3 - "$decoder" "$work" <<'PY'
import importlib.util, sys, glob
spec = importlib.util.spec_from_file_location("fp_sources", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
files = sorted(glob.glob(sys.argv[2] + "/corpus_*.fpo"))
if not files:
    sys.exit("FAIL: no corpus containers for the self-check")

# too LOW: MAD reads three slots; claim it reads one.
mod.ARITY[0x04] = 1
contradicted, _unwitnessed, _seen = mod.verify_arity(files)
if not any(op == 0x04 for _p, op, _s, err in contradicted if err is None):
    sys.exit("FAIL: understating MAD's arity was not reported as CONTRADICTED - "
             "the check cannot see a table that is too low")

# too HIGH: MOV reads one slot; claim it reads two.  This direction is only
# visible as absence of evidence, and that is exactly what must be reported.
mod.ARITY[0x04] = 3
mod.ARITY[0x01] = 2
_contradicted, unwitnessed, _seen = mod.verify_arity(files)
if (0x01, 1) not in unwitnessed:
    sys.exit("FAIL: overstating MOV's arity was not reported as UNWITNESSED - "
             "the check cannot see a table that is too high")
print("  %-42s %s" % ("verifier self-check", "reports a table too low and a table too high"))
PY

printf 'PASS: fp-sources-test\n'

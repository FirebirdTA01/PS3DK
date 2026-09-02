#!/usr/bin/env bash
# No fragment program may compute a value into a register nothing reads
# again (t_5dc260b0).
#
# WHY THIS IS A WHOLE-CORPUS CHECK AND NOT ONE FIXTURE.  On NV40 the colour
# output IS R0, and lowerStoreOutput says so on some shapes only by PINNING
# the composing writes to register 0 - it emits no output store at all.  A
# pin the allocator declines to honour therefore does not cost an
# optimisation: the colour gets composed in R3, nothing writes R0, and the
# program paints whatever the previous draw left there.  It compiles, it
# keeps its kills, it reports a sane register count, and it exits 0.
#
# Three separate commits in one afternoon each shipped a different set of
# shaders in exactly that state, and each was caught by a different
# accident - one by the pixel rig, one by a byte-identity column, one by a
# rig row that only moved because the recipe changed.  THE PIXEL RIG CANNOT
# BE RELIED ON HERE: fp_cf_guarded_divide_f composed its colour with three
# of R0's four lanes never written and was judged IDENTICAL, because
# whatever was already in R0 happened to match the expected picture.  An
# off-slot colour is invisible whenever the stale contents agree.
#
# What is deterministic, and what this test asserts: R0 is the only
# register the hardware reads after the program ends, so a write to any
# other register that is never read again is a value computed and thrown
# away.  When that value is the colour, the picture is wrong.  Dead stores
# are merely wasteful in the general case, so the check is exact rather
# than approximate - the one shader that has always had one is named below
# rather than excused by a loosened rule.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-colour-reaches-r0-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Tracked fragment shaders only: the check has to run in CI, where
# build/shader-corpus does not exist.
shaders=()
while IFS= read -r f; do
    case "$f" in
        *.fcg)      shaders+=("$f") ;;
        *_f.cg)     shaders+=("$f") ;;
    esac
done < <(cd "$repo_root" && git ls-files \
            'tools/rsx-cg-compiler/tests/shaders/*' \
            'tests/regression/*/shaders/*' \
            'samples/*/shaders/*')

(( ${#shaders[@]} > 20 )) || fail "only ${#shaders[@]} tracked fragment shaders
found - the enumeration broke, so nothing below was actually checked"

for s in "${shaders[@]}"; do
    out="$work/$(printf '%s' "$s" | tr '/' '_').log"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx "$repo_root/$s"
    ) >"$out" 2>&1 || rm -f "$out"    # a refusal is not this test's business
done

# ... except for these, where a refusal IS the regression.
#
# Both store the colour more than once, so lowerStoreOutput pins a
# DIFFERENT virtual register to slot 0 per store.  A slot reservation that
# remembers only one owner makes the other reject its own slot, and the
# honour-or-refuse rule then turns that into a named refusal of a shape
# that compiles correctly on every binary before it.  The refusal is the
# honest failure of a wrong reservation - which is exactly why it needs a
# fixture: without one, the wrong reservation looks like a clean run.
for s in tools/rsx-cg-compiler/tests/shaders/fp_output_restored_f.cg \
         tools/rsx-cg-compiler/tests/shaders/fp_output_stored_thrice_f.cg; do
    [[ -f "$repo_root/$s" ]] || fail "fixture missing: $s"
    out="$work/$(printf '%s' "$s" | tr '/' '_').log"
    [[ -s "$out" ]] || fail "$s did not compile.  It stores the colour more
than once, so it pins two colour values to the same output slot; a slot
reserved for only one of them refuses the other (t_5dc260b0)"
done

python3 - "$work" <<'PY'
import os
import re
import sys

# hw[0]: bit 0 end, bits 1..6 destination register, bit 7 destination is an
# H register (two share one R slot), bits 9..12 write mask, bit 30 no
# destination.  hw[1..3]: bits 0..1 register type (0 temp), bits 2..7 the
# register, bit 8 H register.
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")

# The one shader that has always carried a dead write, named rather than
# excused by a weaker rule: its guard lowering leaves a trailing ADD whose
# result nothing reads.  The colour is already complete in R0 before it, so
# the picture is right and the instruction is merely wasted.  If a lowering
# change removes it, delete this entry - do not widen the rule.
KNOWN = {"fp_discard_not_f.cg"}


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def dst_slot(w):
    if w[0] & (1 << 30):
        return None
    reg = (w[0] >> 1) & 0x3F
    return (reg >> 1) if (w[0] & (1 << 7)) else reg


def src_slots(w):
    out = set()
    for s in (1, 2, 3):
        if (w[s] & 3) != 0:
            continue
        reg = (w[s] >> 2) & 0x3F
        out.add((reg >> 1) if (w[s] & (1 << 8)) else reg)
    return out


work = sys.argv[1]
checked, bad, stale = 0, [], []
for name in sorted(os.listdir(work)):
    words = []
    for line in open(os.path.join(work, name), encoding="utf-8"):
        m = LINE.match(line)
        if m:
            w = [unswap(int(x, 16)) for x in m.group(2).split()]
            if len(w) == 4:
                words.append(w)
    if not words:
        continue
    checked += 1
    instrs, i = [], 0
    while i < len(words):
        w = words[i]
        # A source of register type CONST names an inline constant block:
        # sixteen bytes of DATA that must not be walked as an opcode.
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)
        instrs.append(w)
        i += 1 + (1 if consts else 0)

    dead = []
    for n, w in enumerate(instrs):
        slot = dst_slot(w)
        if slot is None or slot == 0:
            continue
        if not any(slot in src_slots(later) for later in instrs[n + 1:]):
            dead.append((n, slot))

    # log name is the repo-relative path with '/' -> '_', plus '.log'
    shader = name.split("shaders_")[-1]
    if shader.endswith(".log"):
        shader = shader[:-len(".log")]
    if dead and shader not in KNOWN:
        bad.append((shader, dead))
    if not dead and shader in KNOWN:
        stale.append(shader)

if checked < 20:
    raise SystemExit("FAIL: only %d shaders produced a ucode dump - the "
                     "compile step broke, so nothing was checked" % checked)

for shader, dead in bad:
    sys.stderr.write(
        "FAIL: %s writes %s and never reads it again.  R0 is the only "
        "register the hardware reads after the program ends, so this is a "
        "value computed and thrown away - and when it is the colour, the "
        "program paints whatever was already in R0 (t_5dc260b0).\n"
        % (shader, ", ".join("R%d at instruction %d" % (s, n)
                             for n, s in dead)))
if bad:
    raise SystemExit(1)

if stale:
    raise SystemExit(
        "FAIL: %s no longer has a dead write, so its entry in this test's "
        "KNOWN set is stale.  Remove the entry - an allowance nobody "
        "revisits is how the next one hides." % ", ".join(stale))

print("colour-reaches-r0: %d fragment shaders, no dead register writes "
      "outside the %d known" % (checked, len(KNOWN)))
PY

printf 'PASS: colour-reaches-r0-test\n'

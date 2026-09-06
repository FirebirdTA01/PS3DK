#!/usr/bin/env bash
# Fragment DEPTH export contract (t_1722b8bc).
#
# EVERY NUMBER BELOW WAS READ OUT OF THE REFERENCE'S OWN OUTPUT, not
# derived from our model of what a depth export should look like.  The
# reference builds fp_depth_export_f.cg as:
#
#     MOVR R0, f[TEX0];
#     MULR R1.z, R0.x, 0.5;
#     END
#
# instructionCount 3, registerCount 2, and its container carries:
#   - depthReplace = 1        (FP program header, offset +20)
#   - depth parameter res     = 0x0b75  (CG_DEPTH0)
#   - depth parameter CGtype  = 0x0417  (CG_FLOAT3 — the reference widens
#                                        the `float` the source declares)
#
# WHY THIS NEEDS ITS OWN TEST.  All four divergences were silent: exit 0,
# a container written, a sane register count, and no instrument aimed at
# any of them.  The pixel rig judges colour only, so a depth export can go
# to the wrong lane forever without moving a single row.  colour-reaches-r0
# saw the R1 write but could only call it a dead store, because its model
# said R0 was the only register the hardware reads after the program ends.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

# Two spellings, because the container builds the parameter table at two
# separate sites: a struct return and an `out` parameter list.  Fixing only
# the site the struct fixture reached left the `out` spelling emitting
# CG_FLOAT, so both are pinned.
fixtures=(fp_depth_export_f fp_depth_export_outparam_f)
for stem in "${fixtures[@]}"; do
    [[ -f "$repo_root/tools/rsx-cg-compiler/tests/shaders/$stem.cg" ]] ||
        fail "fixture missing: $stem.cg"
done

work="${TMPDIR:-/tmp}/ps3dk-depth-export-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# --emit-container suppresses the textual ucode listing, so the container
# and the listing need separate invocations.  The listing run is what the
# R1.z assertion reads; an empty one would make that assertion vacuous, so
# it is checked for content here rather than trusted below.
for stem in "${fixtures[@]}"; do
    src="$repo_root/tools/rsx-cg-compiler/tests/shaders/$stem.cg"

    # DEFAULT (general) path: must compile and honour the whole contract.
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$src"
    ) >"$work/$stem.container.log" 2>&1 || {
        tail -n 20 "$work/$stem.container.log" >&2
        fail "$stem did not compile on the default path"
    }
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$work/$stem.log" 2>&1 || {
        tail -n 20 "$work/$stem.log" >&2
        fail "$stem did not compile (ucode listing run)"
    }
    grep -qE '^\s*[0-9]+:(\s+[0-9a-fA-F]{8})+\s*$' "$work/$stem.log" || fail \
        "$stem emitted no ucode listing - the R1.z check below would pass
vacuously, so the harness is refusing rather than reporting a green"

    # LEGACY path: the retired shape matcher never lowered depth and used
    # to DROP it silently.  It now refuses, and a refusal must not leave a
    # container behind for a caller to pick up and ship.
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx --legacy-lowering \
            --emit-container "$work/$stem.legacy.fpo" "$src"
    ) >"$work/$stem.legacy.log" 2>&1 || rc=$?
    [[ "$rc" -ne 0 ]] || fail \
        "$stem compiled on the legacy path, which has no depth lowering -
it drops the write silently, so it must refuse"
    [[ ! -e "$work/$stem.legacy.fpo" ]] || fail \
        "$stem left a container behind after the legacy refusal"
    grep -q "fragment DEPTH output is not lowered on the legacy path" \
        "$work/$stem.legacy.log" || {
        tail -n 20 "$work/$stem.legacy.log" >&2
        fail "$stem refused on the legacy path for another reason"
    }
done

python3 - "$work" "${fixtures[@]}" <<'PY'
import re
import struct
import sys

work = sys.argv[1]


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


# Same decode as colour-reaches-r0-test.sh: hw[0] bit 30 no destination,
# bits 1..6 destination register, bit 7 destination is an H register,
# bits 9..12 write mask.
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")

Z = 1 << 2


def writes(log):
    words = []
    for line in open(log, encoding="utf-8"):
        m = LINE.match(line)
        if not m:
            continue
        w = [unswap(int(x, 16)) for x in m.group(2).split()]
        if len(w) == 4:
            words.append(w)

    # Walk instructions the way colour-reaches-r0-test.sh does: a source of
    # register type CONST names an inline constant block, sixteen bytes of
    # DATA that must be SKIPPED rather than decoded as an opcode.  Walking
    # every line would let a constant whose bits happen to look like a write
    # to R1.z satisfy the lane assertion below on its own.
    out = []
    i = 0
    while i < len(words):
        w = words[i]
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)
        i += 1 + (1 if consts else 0)
        if w[0] & (1 << 30):
            continue
        reg = (w[0] >> 1) & 0x3F
        slot = (reg >> 1) if (w[0] & (1 << 7)) else reg
        out.append((slot, (w[0] >> 9) & 0xF))
    return out


def container(path):
    b = open(path, "rb").read()
    prog = struct.unpack_from(">I", b, 20)[0]
    # parameterArray is a header FIELD.  Assuming the array follows the
    # header at 0x20 happens to be true today; if a layout change ever
    # moves it this would report "no parameter carries DEPTH" for a
    # reason that has nothing to do with depth.
    o = struct.unpack_from(">I", b, 16)[0]
    cnt = struct.unpack_from(">I", b, 12)[0]
    depth = None
    for i in range(cnt):
        rec = o + 48 * i
        typ, res, _ = struct.unpack_from(">III", b, rec)
        semoff = struct.unpack_from(">I", b, rec + 28)[0]
        sem = ""
        if 0 < semoff < len(b):
            sem = b[semoff:b.index(b"\0", semoff)].decode()
        if sem.upper().startswith("DEPTH"):
            depth = (typ, res)
    return b[prog + 20], depth


problems = []

for tag in sys.argv[2:]:
    depth_replace, depth_param = container("%s/%s.fpo" % (work, tag))

    if depth_param is None:
        problems.append("%s: no container parameter carries a DEPTH semantic"
                        % tag)
    else:
        typ, res = depth_param
        if res != 0x0B75:
            problems.append(
                "%s: depth parameter resource is 0x%04x, reference emits "
                "0x0b75 (CG_DEPTH0); 0 is what the disassembler prints as "
                "'???'" % (tag, res))
        if typ != 0x0417:
            problems.append(
                "%s: depth parameter CGtype is 0x%04x, reference emits 0x0417 "
                "(CG_FLOAT3 - it widens the declared float)" % (tag, typ))

    if depth_replace != 1:
        problems.append(
            "%s: depthReplace is %d, reference emits 1.  The RSX is never "
            "told to use the depth the program writes, so the export is "
            "inert however correct the ucode is." % (tag, depth_replace))

    w = writes("%s/%s.log" % (work, tag))
    r1 = [mask for slot, mask in w if slot == 1 and mask]
    if not r1:
        problems.append(
            "%s: nothing writes R1 at all - the depth computation was "
            "dropped entirely, not merely misplaced." % tag)
    else:
        if not any(mask & Z for mask in r1):
            problems.append(
                "%s: R1 is written with mask(s) %s but never the z lane; the "
                "reference emits MULR R1.z and the hardware reads depth from "
                "R1.z." % (tag, ", ".join("0x%x" % m for m in r1)))

if problems:
    raise SystemExit("FAIL: depth-export\n  " + "\n  ".join(problems))

print("depth export: R1.z, depthReplace=1, CG_DEPTH0/FLOAT3 on both paths")
PY

printf 'PASS: depth-export-test\n'

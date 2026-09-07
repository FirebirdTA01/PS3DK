"""Check SelPred allocation records, never sources belonging to later records.

t_3603033d: leaving a SelPred open across another instruction both invented
aliases with its consumers and hid real aliases by overwriting its sources.
The compiler has a separate final-allocation guard; this checks its trace.
"""
import re
import sys

log_path = sys.argv[1]
alloc_re = re.compile(
    r"alloc\[(\d+)\] op=\d+ opName=SelPred dstOut=(\d+) dstIdx=(\d+) "
    r"dstPhys=(-?\d+) dstFp16=(\d+)"
)
src_re = re.compile(
    r"\s+src([012]) kind=(\d+) idx=(\d+) phys=(-?\d+) fp16=(\d+)"
)

selpreds = []
current = None
with open(log_path, encoding="utf-8", errors="replace") as handle:
    for line in handle:
        # Any allocation header ends the previous record, even if this
        # instruction is not a SelPred and is otherwise irrelevant here.
        if line.startswith("alloc["):
            current = None
        m = alloc_re.match(line)
        if m:
            current = {
                "instr": int(m.group(1)),
                "dst_phys": int(m.group(4)),
                "dst_fp16": int(m.group(5)) != 0,
                "srcs": {},
            }
            selpreds.append(current)
            continue
        m = src_re.match(line)
        if current is not None and m:
            current["srcs"][int(m.group(1))] = {
                "kind": int(m.group(2)),
                "idx": int(m.group(3)),
                "phys": int(m.group(4)),
                "fp16": int(m.group(5)) != 0,
            }

if not selpreds:
    raise SystemExit("FAIL: fixture compiled without any SelPred allocation trace")

def slot(phys, fp16):
    return phys >> 1 if fp16 else phys

checked = 0
for row in selpreds:
    if set(row["srcs"]) != {0, 1, 2}:
        raise SystemExit(f"FAIL: incomplete SelPred allocation record at alloc[{row['instr']}]")
    if row["dst_phys"] < 0:
        raise SystemExit(f"FAIL: SelPred at alloc[{row['instr']}] has unresolved destination")
    dst_slot = slot(row["dst_phys"], row["dst_fp16"])
    if "--distinct-arms" in sys.argv[2:]:
        then, otherwise = row["srcs"][1], row["srcs"][2]
        if (then["kind"] != 1 or otherwise["kind"] != 1 or
                slot(then["phys"], then["fp16"]) ==
                slot(otherwise["phys"], otherwise["fp16"])):
            raise SystemExit("FAIL: distinct branch values lost before SelPred")
    for src_index in (0, 1):
        src = row["srcs"].get(src_index)
        if not src or src["kind"] != 1:
            continue
        if src["phys"] < 0:
            raise SystemExit(
                f"FAIL: SelPred at alloc[{row['instr']}] has unresolved temp src{src_index}"
            )
        checked += 1
        src_slot = slot(src["phys"], src["fp16"])
        if src_slot == dst_slot:
            raise SystemExit(
                f"FAIL: SelPred at alloc[{row['instr']}] writes R{dst_slot} "
                f"but early-read src{src_index} v{src['idx']} also occupies R{src_slot}; "
                "the expanded default MOV would clobber a later source "
                "(t_652d6e42 regression)"
            )

if checked == 0:
    raise SystemExit("FAIL: no temp condition/then sources were checked")

print(f"selpred-output-alias-test: ok ({len(selpreds)} SelPred nodes, {checked} early temp sources)")

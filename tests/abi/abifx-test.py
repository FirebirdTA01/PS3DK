#!/usr/bin/env python3
"""Regression tests for tests/abi/abifx.py (the section-7 strict comparator).

Locks in the behaviours that the old inline set()/sorted() comparator got wrong:

  * a duplicate descriptor must FAIL (set-collapse let it pass);
  * a changed tail addend must FAIL;
  * a changed tail symbol must FAIL;
  * a renamed head symbol is informative, NOT contract -> must still PASS;
  * a head symbol of None vs a name must NOT crash and must PASS (head symbol
    is not compared);
  * an ELF-identity mismatch (machine) must FAIL;
  * the same manifest compared with itself must PASS (both roles);
  * the sprx six-slot contract: single ADDR32 per slot passes; a missing slot
    or a wrong reloc type, or an off-contract reloc, fails.

Run as:        python3 tests/abi/abifx-test.py
Exit status:   0 = every case holds, 1 = at least one failed.
"""
import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import abifx                                  # noqa: E402
FIX = os.path.join(HERE, "fixtures")
CRT0 = os.path.join(FIX, "lv2-crt0.json")
SPRX = os.path.join(FIX, "lv2-sprx.json")

FAILED = []


def case(name, cond):
    print(("  [ok]   " if cond else "  [FAIL] ") + name)
    if not cond:
        FAILED.append(name)


def mut_crt0(fn):
    with open(CRT0) as f:
        d = json.load(f)
    fn(d)
    return d


def run_role(d, role, want_ok, name):
    with open(CRT0 if role == "crt0" else SPRX) as f:
        ref = json.load(f)
    ok, _ = abifx.compare(d, ref, role)
    case(name, ok == want_ok)


def main():
    if not (os.path.exists(CRT0) and os.path.exists(SPRX)):
        print("abifx-test: FAIL — fixtures missing", file=sys.stderr)
        return 1

    print("== crt0 (opd) contract ==")
    # identity + self matches
    with open(CRT0) as f:
        base = json.load(f)
    case("crt0 self-compare passes", abifx.compare(copy.deepcopy(base), copy.deepcopy(base), "crt0")[0])

    # duplicate descriptor -> FAIL  (finding 5: set() collapse let it pass)
    def dup(d):
        descs = d["opd"]["descriptors"]
        d["opd"]["descriptors"] = descs + [copy.deepcopy(descs[0])]
        d["opd"]["entries"] = len(d["opd"]["descriptors"])
    run_role(mut_crt0(dup), "crt0", False, "duplicate descriptor FAILS")

    # a descriptor's offset changed (e.g. 0 -> 4) must FAIL (gap: offset was
    # not part of the contract, so two objects with the same descriptors at
    # different strides compared equal)
    def bad_offset(d):
        d["opd"]["descriptors"][0]["offset"] = 4
    run_role(mut_crt0(bad_offset), "crt0", False, "changed descriptor offset FAILS")

    # changed tail addend -> FAIL (finding 4: addend is contract)
    def tail_add(d):
        d["opd"]["descriptors"][0]["tail_relocs"][0]["addend"] = 4
    run_role(mut_crt0(tail_add), "crt0", False, "changed tail addend FAILS")

    # changed tail symbol -> FAIL (finding 4: tail .TOC. is contract)
    def tail_sym(d):
        d["opd"]["descriptors"][0]["tail_relocs"][0]["symbol"] = ".NOT_TOC"
    run_role(mut_crt0(tail_sym), "crt0", False, "changed tail symbol FAILS")

    # renamed head symbol -> PASS (finding 5: head symbol is informative)
    def head_rename(d):
        d["opd"]["descriptors"][0]["head_relocs"][0]["symbol"] = "._renamed"
    run_role(mut_crt0(head_rename), "crt0", True, "renamed head symbol PASSES")

    # head symbol None vs named -> PASS and no crash (finding 3: TypeError)
    def head_none(d):
        d["opd"]["descriptors"][0]["head_relocs"][0]["symbol"] = None
    run_role(mut_crt0(head_none), "crt0", True, "head symbol None PASSES (no crash)")

    # ELF identity mismatch -> FAIL
    def bad_machine(d):
        d["elf"]["machine"] = "i386"
    run_role(mut_crt0(bad_machine), "crt0", False, "ELF machine mismatch FAILS")

    print("== sprx (.sys_proc_prx_param) contract ==")
    with open(SPRX) as f:
        sprx_base = json.load(f)
    case("sprx self-compare passes", abifx.compare(copy.deepcopy(sprx_base), copy.deepcopy(sprx_base), "sprx")[0])

    def sprx_mut(fn):
        with open(SPRX) as f:
            d = json.load(f)
        fn(d)
        return d

    def sprx_case(mut, want_ok, name):
        run_role(sprx_mut(mut), "sprx", want_ok, name)

    # a slot's ADDR32 missing (drop the last reloc) -> FAIL
    def drop_slot(d):
        d["sys_proc_prx_param"]["relocs"] = d["sys_proc_prx_param"]["relocs"][:-1]
    sprx_case(drop_slot, False, "sprx missing slot FAILS")

    # a slot's reloc type wrong -> FAIL
    def wrong_type(d):
        d["sys_proc_prx_param"]["relocs"][0]["r_type"] = "R_PPC64_REL24"
    sprx_case(wrong_type, False, "sprx wrong slot reloc type FAILS")

    # a slot's symbol renamed is informative -> PASS (spec §3 rule 2)
    def rename_slot_sym(d):
        d["sys_proc_prx_param"]["relocs"][0]["symbol"] = "._renamed_slot"
    sprx_case(rename_slot_sym, True, "sprx renamed slot symbol PASSES")

    # a slot's addend changed -> FAIL (addend is contract)
    def change_addend(d):
        d["sys_proc_prx_param"]["relocs"][0]["addend"] = 8
    sprx_case(change_addend, False, "sprx slot addend change FAILS")

    # a reloc outside the six required offsets -> FAIL (unexpected_relocs)
    def off_reloc(d):
        d["sys_proc_prx_param"]["relocs"].append(
            {"offset": 224, "r_type": "R_PPC64_ADDR32", "symbol": "._extra", "addend": 0})
    sprx_case(off_reloc, False, "sprx off-contract reloc FAILS")

    if FAILED:
        print("abifx-test: FAIL (%d case(s))" % len(FAILED), file=sys.stderr)
        for name in FAILED:
            print("  - %s" % name, file=sys.stderr)
        return 1
    print("abifx-test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
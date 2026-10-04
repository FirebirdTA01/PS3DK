#!/usr/bin/env python3
"""Check the hybrid relative-table dispatch, without executing target code.

This intentionally checks our canonicalization contract, not arbitrary GCC
code sequences. Unknown or missing dispatch shapes fail rather than pass.
--assembly accepts retained assembly; --compiler builds fresh O2/O3 controls.
Runtime execution and physical execution-mode validation are separate gates.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile


def check(text, canonical=True):
    insns = []
    for line in text.splitlines():
        match = re.match(r"^\s+([a-z][a-z0-9.]*)\s*(.*?)\s*$", line)
        if match:
            insns.append((match[1], match[2].replace(" ", "")))
        elif line.strip().endswith(":"):
            insns.append(("label", line.strip()))
    dispatches = 0
    for i, (op, operands) in enumerate(insns):
        if op != "mtctr" or i + 1 >= len(insns):
            continue
        # Normal and speculation-barrier tablejump forms.
        tail = insns[i + 1:i + 4]
        if not (tail[0][0] == "bctr"
                or (len(tail) >= 2 and tail[0][0] == "crset"
                    and tail[1][0].startswith("beqctr"))):
            continue
        window = insns[max(0, i - 8):i]
        # A relative dispatch has a table-word load and address add.
        if not any(x[0] in ("lwzx", "lwax") for x in window):
            continue
        if not any(x[0] == "add" for x in window):
            continue
        dispatches += 1
        # Follow the canonical target even when register allocation copies it
        # to a different GPR before mtctr.
        target = operands
        protected = False
        producer = None
        for position in range(len(window) - 1, -1, -1):
            prev, args = window[position]
            if prev == "label":
                break
            fields = args.split(",")
            if fields[0] != target:
                continue
            if prev == "mr" and len(fields) == 2:
                target = fields[1]
                continue
            if (prev == "rldicl" and len(fields) == 4
                    and fields[2:] == ["0", "32"] and not protected):
                protected = True
                target = fields[1]
                continue
            if prev == "add" and len(fields) == 3:
                producer = (position, fields[1:])
            break
        if producer is None:
            raise AssertionError(f"dispatch {dispatches}: target does not trace to the table add")
        position, sources = producer

        def reaching_load(reg, before, signed=False):
            for at in range(before - 1, -1, -1):
                prev, args = window[at]
                if prev == "label":
                    return None
                fields = args.split(",")
                if fields[0] != reg:
                    continue
                if prev == "mr" and len(fields) == 2:
                    return reaching_load(fields[1], at, signed)
                if prev == "extsw" and len(fields) == 2:
                    return reaching_load(fields[1], at, True)
                if prev in ("lwzx", "lwax"):
                    return signed or prev == "lwax"
                # A prior table load cannot reach the add through this write.
                return None
            return None

        loads = [reaching_load(source, position) for source in sources]
        if all(load is None for load in loads):
            raise AssertionError(f"dispatch {dispatches}: add does not consume the table load")
        if not canonical and not any(load is True for load in loads):
            raise AssertionError(f"dispatch {dispatches}: LP64 displacement is not sign-extended")
        if protected != canonical:
            raise AssertionError(f"dispatch {dispatches}: canonical={protected}, expected {canonical}")
    if not dispatches:
        raise AssertionError("no recognized relative table dispatch (vacuous control)")
    return dispatches


def selftest():
    prefix = "\tlwzx 9,10,14\n\tadd 9,9,10\n"
    suffix = "\tmtctr 9\n\tbctr\n"
    good = prefix + "\trldicl 9,9,0,32\n" + suffix
    assert check(good) == 1
    assert check(prefix.replace("lwzx", "lwax") + suffix, False) == 1
    assert check(prefix.replace("\tadd", "\textsw 9,9\n\tadd") + suffix, False) == 1
    bad = [prefix + suffix, good.replace("0,32", "0,31"),
           good.replace("rldicl 9,9", "rldicl 8,9"),
           good.replace("rldicl 9,9", "rldicl 9,8"),
           good.replace("\tadd", "\tli 9,123\n\tadd"),
           good.replace("\tmtctr", "\tadd 9,9,10\n\tmtctr"), "\tblr\n"]
    for mutant in bad:
        try:
            check(mutant)
        except AssertionError:
            continue
        raise AssertionError("checker accepted a negative control")
    for mutant in (prefix + suffix,
                   prefix.replace("lwzx", "lwax").replace("\tadd", "\tli 9,123\n\tadd") + suffix):
        try:
            check(mutant, False)
        except AssertionError:
            continue
        raise AssertionError("checker accepted an unsigned or overwritten LP64 displacement")
    print("checker controls: 12/12")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assembly", type=pathlib.Path)
    parser.add_argument("--compiler")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
    if args.assembly:
        print("relative dispatches:", check(args.assembly.read_text()))
    if args.compiler:
        source = pathlib.Path(__file__).with_name("ppu-relative-switch.c")
        with tempfile.TemporaryDirectory(prefix="ppu-switch-") as tmp:
            for opt in ("-O2", "-O3"):
                for barrier in ("-mspeculate-indirect-jumps", "-mno-speculate-indirect-jumps"):
                    for lp64 in (False, True):
                        out = pathlib.Path(tmp) / "switch.s"
                        argv = [args.compiler, opt, "-mcpu=cell", "-mrelative-jumptables", barrier]
                        if lp64:
                            argv.append("-mlp64")
                        argv += ["-S", str(source), "-o", str(out)]
                        subprocess.run(argv, check=True, timeout=60)
                        count = check(out.read_text(), canonical=not lp64)
                        print(opt, barrier, "LP64" if lp64 else "ILP32", count, "PASS")
    if not (args.selftest or args.assembly or args.compiler):
        parser.error("select --selftest, --assembly or --compiler")


if __name__ == "__main__":
    main()

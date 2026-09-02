#!/usr/bin/env python3
"""shader-differential: the N-live-registers family (t_5dc260b0), 2026-09-02.

Two readouts: the FIRST REFUSAL per compiler (binary; the acceptance gate) and
the SLOPE of emitted instructions per term (the gradient; the progress
measure on a compiler that never refuses - the vita team's addition, 2026-09-02).

Generates fragment shaders with N float4 terms, each derived from the
inputs by a distinct affine op, summed into the output:

    float4 t_i = c * k_i + d.<lane> * m_i;   (i = 0..N-1)
    o = t_0 + t_1 + ... + t_{N-1};

The sum is foldable term by term, so a lowering that folds as it goes
keeps a handful of values live whatever N is, and one that materialises
every term before the first add keeps N live.  Compiling the family with
OUR compiler on both paths and with the reference gives the register
bound as a NUMBER instead of the 48 that gets cited: the first N each
compiler refuses at.

Measured 2026-09-02 on 70568e9: reference ok for every N through 64;
ours general ok through 12 and refuses at 16 with the register
diagnostic; ours default refuses every N >= 2 (an unlowered arithmetic
shape).  So the corpus's register-pressure refusals are liveness the
lowering creates, not the budget.

Usage (from Windows, the compiler living in WSL as everywhere in the rig):
    python n-live.py --ours /tmp/<build>/rsx-cg-compiler [--reference <sce-cgc.exe>]
                     [--out <dir>] [--max 64]
No boot: this is a compile-only instrument.  Exit code 0 always; the
table is the result.
"""
import argparse
import os
import re
import subprocess
import sys

STEPS = [1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64]


def shader(n):
    lines = ["void main(float4 c : TEXCOORD0, float4 d : TEXCOORD1, out float4 o : COLOR)", "{"]
    for i in range(n):
        lines.append("    float4 t%d = c * %.3f + d.%s * %.4f;" % (i, 1.0 + i * 0.125, "xyzw"[i % 4], 0.5 + i * 0.0625))
    lines.append("    o = " + " + ".join("t%d" % i for i in range(n)) + ";")
    lines.append("}")
    return "\n".join(lines) + "\n"


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def classify(rc, text):
    if rc == 0:
        return "ok"
    for key in ("register", "budget", "spill", "did not lower", "unsupported"):
        if key in text:
            return "refuse(%s)" % key
    return "refuse(rc=%d)" % rc


INSN_LINE = re.compile(r"^\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")


def count_insn(text):
    """16-byte groups in the compiler's ucode dump (the 'N: w0 w1 w2 w3'
    lines), inline const blocks INCLUDED - the container's instructionCount
    semantics, so the column is like for like with the reference's field.
    The first refusal is binary and says nothing on a compiler that never
    refuses; the SLOPE of instructions per term is the finding there (the
    vita team's addition; on this measure ours reads 6.75 per term against
    the reference's 5.25 at N 8..12, both linear)."""
    n = 0
    for line in text.splitlines():
        m = INSN_LINE.match(line)
        if not m:
            continue
        words = [int(w, 16) for w in m.group(2).split()]
        if len(words) < 4:
            continue
        # Counted WITH inline const blocks: that is the container's
        # instructionCount semantics (ucode/16), which the reference's field -
        # read by reference_insn - also carries, so the two columns are like
        # for like (claude, review of a0cd14f: excluding them here read 59
        # against a reference 63 whose true like-for-like is 81).
        n += 1
    return n


def reference_insn(reference_exe, container):
    """instructionCount from the reference's own disassembler, if it sits
    beside the reference compiler; None otherwise."""
    d = os.path.join(os.path.dirname(reference_exe), "sce-cgcdisasm.exe")
    if not os.path.exists(d) or not os.path.exists(container):
        return None
    rc, text = run([d, container])
    m = re.search(r"instructionCount\s+(\d+)", text)
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True, help="WSL path of rsx-cg-compiler")
    ap.add_argument("--reference", default=os.environ.get("PS3_REF_CG_COMPILER", ""), help="reference compiler exe (optional)")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "sd-n-live"))
    ap.add_argument("--max", type=int, default=64)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    steps = [n for n in STEPS if n <= a.max]
    rows = []
    for n in steps:
        src = os.path.join(a.out, "nlive_%02d_f.cg" % n)
        with open(src, "w", newline="\n") as f:
            f.write(shader(n))
        row = {"N": n}
        for label, flags in (("legacy", ["--legacy-lowering"]), ("general", [])):
            # A bare compile (no --emit-container) prints the ucode dump the
            # instruction count is read from; a refusal still returns non-zero.
            rc, text = run(["wsl", "--", "timeout", "30s", a.ours] + flags +
                           ["-p", "sce_fp_rsx", wsl_path(src)])
            row[label] = classify(rc, text)
            row[label + " insn"] = count_insn(text) if rc == 0 else "-"
        if a.reference:
            ref_out = os.path.join(a.out, "ref_%02d.fpo" % n)
            rc, text = run([a.reference, "-p", "sce_fp_rsx", "-o", ref_out, src])
            row["reference"] = "ok" if rc == 0 else "refuse"
            ri = reference_insn(a.reference, ref_out) if rc == 0 else None
            row["reference insn"] = ri if ri is not None else "-"
        rows.append(row)
    cols = ["N", "legacy", "legacy insn", "general", "general insn"] + (["reference", "reference insn"] if a.reference else [])
    print("n-live: ours = wsl:%s%s" % (a.ours, ", reference present" if a.reference else ", no reference"))
    print("  ".join("%-22s" % c for c in cols))
    for row in rows:
        print("  ".join("%-22s" % str(row[c]) for c in cols))
    verdict_cols = [c for c in cols[1:] if not c.endswith(" insn")]
    first = {c: next((r["N"] for r in rows if r[c] != "ok"), None) for c in verdict_cols}
    print("first refusal: " + ", ".join("%s=%s" % (c, first[c] if first[c] else "none through %d" % steps[-1]) for c in verdict_cols))
    # the slope: instructions per term between the two largest N that compiled
    for c in verdict_cols:
        pts = [(r["N"], r[c + " insn"]) for r in rows if isinstance(r[c + " insn"], int)]
        if len(pts) >= 2:
            (n1, i1), (n2, i2) = pts[-2], pts[-1]
            print("slope %s: %.2f instructions per term (N %d..%d)" % (c, (i2 - i1) / float(n2 - n1), n1, n2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

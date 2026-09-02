#!/usr/bin/env python3
"""Compile every shader in a corpus with rsx-cg-compiler and report what fails.

A regression gate: it answers one question - does our Cg compiler still accept
the shaders we say it accepts - and exits non-zero when the answer changes.
Run it over the checked-in corpus in CI, or point it at a larger local corpus
to measure coverage.

  scripts/shader-compile-check.py --out build/shader-check
  scripts/shader-compile-check.py --out build/shader-check --general-lowering
  scripts/shader-compile-check.py --out build/shader-check \\
      --external-corpus build/shader-corpus --min-shaders 100

Emits results.csv, one fixed-column row per shader: what was tried, whether it
compiled, the container size, and the diagnostic if it did not.
"""

import argparse
import csv
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SHADER_SUFFIXES = (".cg", ".vcg", ".fcg")
DEFAULT_ROOTS = ["tools/rsx-cg-compiler/tests/shaders", "samples"]


def find_compiler(explicit):
    for candidate in (explicit, "rsx-cg-compiler.exe", "rsx-cg-compiler"):
        if not candidate:
            continue
        p = Path(candidate)
        if p.exists():
            return p
        found = shutil.which(p.name)
        if found:
            return Path(found)
    return None


def infer_profile(path):
    """fragment/vertex from the extension, else from the _f/_v name suffix.

    Undecidable is reported rather than guessed: compiling a vertex shader as a
    fragment program produces a diagnostic that reads like a compiler bug.
    """
    ext = path.suffix.lower()
    if ext == ".fcg":
        return "fp"
    if ext == ".vcg":
        return "vp"
    stem = path.stem.lower()
    if stem.endswith("_f"):
        return "fp"
    if stem.endswith("_v"):
        return "vp"
    return None


def discover_tracked(roots):
    """Shaders that are CHECKED IN under the given repo-relative roots.

    git ls-files rather than a filesystem walk: this corpus is defined by what
    is committed, and a walk also collects build strays.
    """
    out = []
    for root in roots:
        if not (REPO / root).exists():
            continue
        listing = subprocess.run(
            ["git", "ls-files", str(root)],
            cwd=REPO, capture_output=True, text=True, check=True,
        ).stdout.splitlines()
        out.extend(REPO / rel for rel in listing
                   if Path(rel).suffix.lower() in SHADER_SUFFIXES)
    return sorted(set(out))


def discover_external(roots):
    """Shaders under a fetched corpus, by filesystem walk.

    Deliberately not git ls-files: a fetched corpus is gitignored by design, so
    asking "is it checked in" returns nothing and would report an empty corpus
    rather than an error.  Kept as its own flag instead of a fallback, so a
    silent switch between the two cannot hide an empty result.
    """
    out = []
    for root in roots:
        base = Path(root)
        if not base.is_absolute():
            base = REPO / root
        if not base.exists():
            continue
        for p in base.rglob("*"):
            # Fetcher scratch checkouts hold whole upstream trees - submodule
            # stubs and dangling links the OS refuses to stat.
            if "_work" in p.parts:
                continue
            try:
                if p.is_file() and p.suffix.lower() in SHADER_SUFFIXES:
                    out.append(p)
            except OSError:
                continue
    return sorted(set(out))


def diagnostic(text, ok, limit=240):
    """The line a human needs, not the first line the tool printed.

    The compiler opens every run - success or failure - with a banner naming
    the entry point and stage, so summarizing by FIRST line reports the word
    "main" for a failure and hides what it actually said.
    """
    lines = [ln.strip() for ln in (text or "").splitlines() if ln.strip()]
    if not lines:
        return ""
    return lines[0][:limit] if ok else " | ".join(lines[-2:])[:limit]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--ours", help="path to rsx-cg-compiler")
    ap.add_argument("--corpus", action="append", default=[],
                    help="repo-relative root of checked-in shaders (repeatable)")
    ap.add_argument("--external-corpus", action="append", default=[],
                    dest="external", metavar="DIR",
                    help="root of a fetched corpus, walked rather than "
                         "git-listed (repeatable)")
    ap.add_argument("--general-lowering", action="store_true",
                    help="accepted and ignored: the general path is the "
                         "default (removed after one release)")
    ap.add_argument("--legacy-lowering", action="store_true",
                    help="pass --legacy-lowering: compile with the retired "
                         "shape matcher instead of the general path")
    ap.add_argument("--min-shaders", type=int, default=1,
                    help="fail if fewer than this many shaders were found "
                         "(default 1)")
    args = ap.parse_args()

    compiler = find_compiler(args.ours)
    if compiler is None:
        sys.exit("rsx-cg-compiler not found: pass --ours <path>")

    roots = args.corpus if (args.corpus or args.external) else DEFAULT_ROOTS
    corpus = discover_tracked(roots) + discover_external(args.external)

    # A compile gate over an empty corpus passes trivially, which is the one
    # way this script could report success while testing nothing at all.  Make
    # that a hard failure rather than a green run, and let CI pin the expected
    # floor so a corpus that quietly shrinks is caught too.
    if len(corpus) < args.min_shaders:
        sys.exit("found {} shaders, expected at least {} - refusing to report "
                 "a pass over an empty or shrunken corpus".format(
                     len(corpus), args.min_shaders))

    outdir = Path(args.out).resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    print("compiler : {}".format(compiler))
    print("shaders  : {}".format(len(corpus)))
    print("lowering : {}".format(
        "general (experimental)" if args.general_lowering else "default"))

    rows, failures, skipped = [], [], []
    for src in corpus:
        try:
            rel = src.relative_to(REPO).as_posix()
        except ValueError:
            rel = src.as_posix()

        prof = infer_profile(src)
        if prof is None:
            skipped.append(rel)
            rows.append(dict(shader=rel, profile="?", result="skipped",
                             exit_code="", container_bytes="",
                             diagnostics="profile undecidable from name"))
            continue

        # Short, stable per-shader work dir.  The obvious name - the relative
        # path flattened - exceeds Windows' 260-character limit on a fetched
        # corpus, whose paths are deep before we add our own.
        work = outdir / "work" / "{}_{}".format(
            hashlib.sha256(rel.encode("utf-8")).hexdigest()[:12], src.stem[:32])
        work.mkdir(parents=True, exist_ok=True)
        container = work / "out.bin"

        cmd = [str(compiler), "-p",
               "sce_fp_rsx" if prof == "fp" else "sce_vp_rsx"]
        if args.general_lowering:
            cmd.append("--general-lowering")
        if args.legacy_lowering:
            cmd.append("--legacy-lowering")
        cmd += ["--emit-container", str(container), str(src)]

        try:
            cp = subprocess.run(cmd, cwd=work, capture_output=True, text=True,
                                timeout=120)
            rc, log = cp.returncode, (cp.stdout or "") + (cp.stderr or "")
        except subprocess.TimeoutExpired:
            rc, log = 124, "TIMEOUT"

        ok = rc == 0 and container.exists() and container.stat().st_size > 0
        rows.append(dict(shader=rel, profile=prof,
                         result="pass" if ok else "fail", exit_code=rc,
                         container_bytes=container.stat().st_size if ok else 0,
                         diagnostics=diagnostic(log, ok)))
        if not ok:
            failures.append(rows[-1])

    res_path = outdir / "results.csv"
    with res_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=[
            "shader", "profile", "result", "exit_code", "container_bytes",
            "diagnostics"])
        writer.writeheader()
        writer.writerows(rows)

    print("results  : {} ({} rows)".format(res_path, len(rows)))
    if skipped:
        print("skipped  : {} (profile undecidable)".format(len(skipped)))
    print("failures : {}".format(len(failures)))
    for row in failures[:40]:
        print("  {}: {}".format(row["shader"], row["diagnostics"]))
    if len(failures) > 40:
        print("  ... and {} more, see results.csv".format(len(failures) - 40))

    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()

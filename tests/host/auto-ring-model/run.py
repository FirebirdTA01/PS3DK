#!/usr/bin/env python3
"""Build and run the private report-ring model; retain commands and receipts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
MUTANTS = [
    (1, "no-report-fence", "fences", "report_completion_fence"),
    (2, "no-copy-fence", "fences", "copy_publication_fence"),
    (3, "early-local-reuse", "fences", "local_read_lifetime"),
    (4, "early-ring-reuse", "fences", "destination_lifetime:next"),
    (5, "stale-label", "fences", "stale_label"),
    (6, "silent-ring-full", "capacity", "ring_full_is_visible"),
    (7, "ack-short-write", "fences", "short_sink_retains"),
    (8, "unflushed-seal", "boundaries", "seal_tail_flush:slots"),
    (9, "ignored-replay", "quarantine", "replay_is_not_new_work"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--mutants", action="store_true")
    ap.add_argument("--ring-bytes", type=int, default=16*1024*1024)
    ap.add_argument("--usable-pairs", type=int, default=504)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipts = []
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", PYTHONDONTWRITEBYTECODE="1")

    def run(name, argv):
        result = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=180)
        (out / (name + ".stdout")).write_text(result.stdout)
        (out / (name + ".stderr")).write_text(result.stderr)
        receipts.append(dict(name=name, argv=argv, rc=result.returncode))
        (out / "commands.json").write_text(json.dumps(receipts, indent=2) + "\n")
        return result

    def build(name, mutant=0):
        binary = out / name
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1"]
        if a.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        result = run(name + "-build", shlex.split(os.environ.get("CC", "cc")) + flags +
                     [f"-DMUTANT={mutant}", str(HERE / "ring_model.c"), "-o", str(binary)])
        if result.returncode:
            raise RuntimeError("build failed (not a mutant kill): " + result.stderr)
        return binary

    binary = build("model")
    result = run("baseline", [sys.executable, str(HERE / "test_model.py"), "--binary", str(binary),
                 "--out", str(out / "baseline"), "--ring-bytes", str(a.ring_bytes),
                 "--usable-pairs", str(a.usable_pairs)])
    if result.returncode:
        raise RuntimeError("baseline failed:\n" + result.stdout + result.stderr)
    print(result.stdout, end="", flush=True)
    killed = []
    if a.mutants:
        for number, name, case, assertion in MUTANTS:
            binary = build(name, number)
            result = run(name, [sys.executable, str(HERE / "test_model.py"), "--binary", str(binary),
                               "--out", str(out / (name + "-results")), "--case", case])
            # Only the named semantic assertion is a kill. A traceback,
            # sanitizer failure, crash, timeout or compiler failure is not.
            if result.returncode != 1 or result.stderr or ("ASSERTION: " + assertion) not in result.stdout.splitlines():
                raise RuntimeError(f"mutant {name} not killed by {assertion}:\n{result.stdout}{result.stderr}")
            killed.append(dict(name=name, assertion=assertion))
            print(name + " KILLED: " + assertion, flush=True)
    sources = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in HERE.iterdir() if p.is_file()}
    (out / "sources.json").write_text(json.dumps(sources, indent=2) + "\n")
    (out / "result.json").write_text(json.dumps(dict(result="PASS", sanitize=a.sanitize,
                                    mutants=killed), indent=2) + "\n")
    print(f"report-ring model PASS; {len(killed)} named mutant kills; no target execution")


if __name__ == "__main__":
    main()

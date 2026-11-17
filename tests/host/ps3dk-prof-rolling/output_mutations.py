#!/usr/bin/env python3
"""Accepted-output assertion controls. Caller owns build authority; no checkout edits."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / "sdk/libps3dkprof/src/rolling/roll_output.c"
CASES = [
    ("wrong_trailer_kind", "short", "pfo_accept(f.out,status,24)==PFR_OK", [
        ("#define TRAILER 6u", "#define TRAILER 7u")]),
    ("rotate_before_preflight", "refused", "calls==f.m->writes+f.m->syncs", [
        ("    memset(&prepared,0,sizeof prepared);", "    if(n>o->b.payload_capacity-o->v.pending_bytes){rc=pfo_flush(o);if(rc!=PFR_OK)return rc;}\n    memset(&prepared,0,sizeof prepared);")]),
    ("ack_before_commit", "faults", "a.accepted_next_chunk==b.accepted_next_chunk", [
        ("if(n>o->b.payload_capacity-o->v.pending_bytes){rc=pfo_flush(o);if(rc!=PFR_OK)return rc;}",
         "if(n>o->b.payload_capacity-o->v.pending_bytes){o->v.accepted_next_chunk=next;o->v.accepted_payload_bytes=bytes;rc=pfo_flush(o);if(rc!=PFR_OK)return rc;}")]),
    ("checkpoint_after_payload", "rotations", "r64(seg.checkpoint)==seg.first_chunk", [
        ("pfr_commit(o->store,o->b.checkpoints[0],o->start_bytes,o->b.payload,o->v.pending_bytes)",
         "pfr_commit(o->store,o->b.checkpoints[1],o->accepted_bytes,o->b.payload,o->v.pending_bytes)")]),
    ("ignore_failed_commit", "faults", "pfo_accept(f.out,p,24)==PFR_IO", [
        ("    if(rc!=PFR_OK)return poison(o,rc);\n    rc=pfr_get_view(o->store,&v);",
         "    if(rc!=PFR_OK){o->v.pending_bytes=0;return PFR_OK;}\n    rc=pfr_get_view(o->store,&v);")]),
    ("forget_resource_stop", "refused", "b.phase==PFO_STOPPING", [
        ("o->v.phase=PFO_STOPPING;o->v.reason=reason;", "o->v.reason=reason;")]),
    ("seal_without_trailer", "rotations", "pfo_finish(f.out)==PFR_INVALID", [
        ("if(o->v.phase!=PFO_STOPPING&&!o->v.terminal)return PFR_INVALID;", "/* mutant permits premature seal */")]),
    ("accept_aliased_input", "inputs", "pfo_accept(f.out,f.payload,24)==PFR_INVALID", [
        ("||!separate(o,raw,n)", "||(!separate(o,raw,n)&&!raw)")]),
    ("ignore_checkpoint_growth", "refused", "pfo_accept(f.out,p,24)!=PFR_OK", [
        ("if(prepared.checkpoint_bytes>o->b.checkpoint_capacity)return stopping(o,PFR_CHECKPOINT_BYTES);",
         "if(prepared.checkpoint_bytes>o->b.checkpoint_capacity)prepared.checkpoint_bytes=o->accepted_bytes;")]),
]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=Path, required=True)
    a = p.parse_args()
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    original = SOURCE.read_bytes()
    text = original.decode("utf-8")
    (out / "source.sha256").write_text(hashlib.sha256(original).hexdigest() + "\n", encoding="utf-8")
    try:
        for name, case, witness, edits in [("baseline", None, None, [])] + CASES:
            changed = text
            for before, after in edits:
                if changed.count(before) != 1:
                    raise RuntimeError(f"{name}: mutation anchor is not unique")
                changed = changed.replace(before, after)
            source = out / (name + ".c")
            source.write_text(changed, encoding="utf-8")
            command = [sys.executable, str(HERE / "run_output.py"), "--build-dir", str(out / name), "--source", str(source)]
            if case:
                command += ["--case", case]
            result = subprocess.run(command, capture_output=True, text=True, timeout=100)
            log = result.stdout + result.stderr
            (out / (name + ".log")).write_text(log, encoding="utf-8")
            if name == "baseline":
                if result.returncode != 0 or "PASS output ownership" not in log:
                    raise RuntimeError("baseline failed")
                print("PASS output baseline", flush=True)
            else:
                failures = [line for line in log.splitlines() if line.startswith("FAIL output:")]
                if result.returncode != 1 or len(failures) != 1 or witness not in failures[0]:
                    raise RuntimeError(f"{name}: survived or failed outside its expected assertion")
                print(f"KILLED {name}: {case}: {witness}", flush=True)
    finally:
        if SOURCE.read_bytes() != original:
            raise RuntimeError("checkout source changed")
    print("PASS 9 output admission and publication controls")

if __name__ == "__main__":
    main()

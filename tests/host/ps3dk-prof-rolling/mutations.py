#!/usr/bin/env python3
"""Compile/run targeted physical-store mutants, only when parent authorizes C builds.

Requires a passing baseline. A compiler failure is a harness error, not a killed
mutant. Each mutation describes an actual storage-contract violation.
"""
import argparse
import pathlib
import shlex
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
IMPL = HERE.parents[2] / "sdk/libps3dkprof/src/rolling"
MUTATIONS = [
    ("cap_omits_spare", "((uint64_t)c->retained_slots + 1u) * slot",
     "((uint64_t)c->retained_slots) * slot"),
    ("overwrites_current_oldest", "count = next.view.retained_segments;",
     "count = next.view.retained_segments;\n    if (count == s->cfg.retained_slots) slot = next.entries[0].physical;"),
    ("accepts_bad_chunk_crc", "u32(p + total - 4) != crc32(p, total - 4)", "0"),
    ("ignores_slot_integrity", "|| u32(f + 60) != crc32(f, 60)) return PFR_CORRUPT;",
     "|| u32(f + 60) != crc32(f, 60)) { (void)0; }"),
    ("accepts_incomplete_root_chain", "if (rc != PFR_OK) { valid[k] = 0; break; }",
     "if (rc != PFR_OK) { (void)0; }"),
    ("keeps_writing_after_io_failure", "static int poison(pfr_store *s) { s->failed = 1; return PFR_IO; }",
     "static int poison(pfr_store *s) { (void)s; return PFR_IO; }"),
    ("publishes_root_before_slot_sync",
     "if (io_write(s, slot_offset(s, slot), p, s->layout.slot_bytes) != PFR_OK || s->io.sync(s->io.ctx))",
     "if (io_write(s, slot_offset(s, slot), p, s->layout.slot_bytes) != PFR_OK)"),
]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="cc")
    p.add_argument("--build-dir", type=pathlib.Path, required=True)
    args = p.parse_args()
    source = (IMPL / "roll_store.c").read_text()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cases = [("baseline", source)]
    for name, before, after in MUTATIONS:
        if source.count(before) != 1:
            raise RuntimeError(f"{name}: expected exactly one mutation site")
        cases.append((name, source.replace(before, after)))
    for name, text in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        altered = case / "roll_store.c"
        altered.write_text(text)
        exe = case / "test.exe"
        command = shlex.split(args.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(IMPL), str(HERE / "roll_store_test.c"), str(altered), "-o", str(exe)]
        built = subprocess.run(command, text=True, capture_output=True)
        (case / "compile.log").write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(f"{name}: compiler error (not a mutation kill); see {case / 'compile.log'}")
        ran = subprocess.run([str(exe)], text=True, capture_output=True)
        (case / "run.log").write_text(ran.stdout + ran.stderr)
        if name == "baseline":
            if ran.returncode:
                raise RuntimeError(f"baseline failed; see {case / 'run.log'}")
            print("PASS baseline")
        else:
            if ran.returncode == 0:
                raise RuntimeError(f"SURVIVED {name}")
            print(f"KILLED {name}: {ran.stderr.strip().splitlines()[-1]}")
    print(f"PASS {len(MUTATIONS)} physical-store mutation controls; no semantic-checkpoint claim")

if __name__ == "__main__":
    main()

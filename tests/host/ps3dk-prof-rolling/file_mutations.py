#!/usr/bin/env python3
"""Sensitive guards for positioned rolling IO; caller owns build authority."""
import argparse
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / "sdk/libps3dkprof/src/rolling/roll_file.c"
CASES = [
    ("host_narrows_offset", False, "(off_t)offset", "(off_t)(uint32_t)offset", "b[i]==0"),
    ("lv2_narrows_offset", True, "sysLv2FsLSeek64(f->fd,offset,0,&position)",
        "sysLv2FsLSeek64(f->fd,(uint32_t)offset,0,&position)", "io.write_at(io.ctx,high"),
    ("ignores_extent", False, "if(offset>f->extent || (uint64_t)bytes>f->extent-offset)",
        "if((void)offset,(void)bytes,0)", "io.write_at(io.ctx,cap-1"),
    ("unchecked_resize", False, "if(size!=bytes) return failure(f,EIO);", "if(0) return failure(f,EIO);", "io.resize(io.ctx,4096)"),
    ("unchecked_seek", True, "if(position!=offset) return failure(f,EIO);", "if(0) return failure(f,EIO);", "transfers==before"),
    ("unchecked_count", True, "if(done>(uint64_t)count) return failure(f,EIO);", "if(0) return failure(f,EIO);", "io.write_at(io.ctx,high,b,16)<0"),
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text()
    for name, lv2 in (("host-base", False), ("lv2-base", True)):
        cmd = [sys.executable, str(HERE / "run_file.py"), "--build-dir", str(out / name)]
        if lv2:
            cmd += ["--lv2-mock"]
        subprocess.run(cmd, check=True)
    for name, lv2, old, new, witness in CASES:
        if old not in text:
            raise SystemExit(f"missing mutation site: {name}")
        case = out / name
        case.mkdir()
        source = case / "roll_file.c"
        source.write_text(text.replace(old, new))
        cmd = [sys.executable, str(HERE / "run_file.py"), "--build-dir", str(case), "--source", str(source)]
        if lv2:
            cmd += ["--lv2-mock"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=45)
        log = result.stdout + result.stderr
        (case / "result.log").write_text(log)
        if result.returncode != 1 or "FAIL file:" not in log or witness not in log:
            raise SystemExit(f"mutant not rejected by its assertion: {name}\n{log}")
        print(f"KILLED {name}")
    print(f"PASS {len(CASES)} positioned IO guard controls")

if __name__ == "__main__":
    main()

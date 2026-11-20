#!/usr/bin/env python3
"""Build/run independent real-SPU mixed source and validate ordinary reader first.

No rolling encoder is linked or invoked. Parent owns compiler/reader runs.
The separate Rust artifact test checks exact producer truth and provenance.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--reader', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    artifact = out / 'original'
    artifact.mkdir()
    spu = ROOT / 'sdk/libps3dkprof/src/spu'
    model = HERE.parent / 'ps3dk-prof-spu'
    exe = out / ('combined-spu-source.exe' if sys.platform == 'win32' else 'combined-spu-source')
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-I', str(spu), '-I', str(model)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    # combined_spu_source.c includes unchanged model.c once for its port shims.
    sources = [HERE / 'combined_spu_source.c', spu / 'psp_spu.c', spu / 'psp_ppu.c', model / 'order_model.c']
    subprocess.run(flags + list(map(str, sources)) + ['-o', str(exe)], check=True, timeout=180)
    generated = subprocess.run([str(exe), str(artifact)], capture_output=True, text=True, timeout=120)
    (out / 'producer.stdout').write_text(generated.stdout)
    (out / 'producer.stderr').write_text(generated.stderr)
    print(generated.stdout, end='', flush=True)
    if generated.returncode:
        print(generated.stderr, file=sys.stderr)
        return generated.returncode
    checked = subprocess.run([str(a.reader.resolve()), 'check', str(artifact / 'source.prof')],
                             capture_output=True, text=True, timeout=120)
    (out / 'ordinary-reader.stdout').write_text(checked.stdout)
    (out / 'ordinary-reader.stderr').write_text(checked.stderr)
    print(checked.stdout, end='', flush=True)
    if checked.returncode:
        print(checked.stderr, file=sys.stderr)
        return checked.returncode
    print(f'PS3DK_PROF_ROLLING_COMBINED_SOURCE_DIR={artifact}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

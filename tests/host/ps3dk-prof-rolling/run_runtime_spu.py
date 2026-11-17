#!/usr/bin/env python3
"""Build D6 actual runtime SPU witnesses; parent alone owns compiler authority.

--red-stubs adds only the rejecting combined-selector API. The old D5 runtime,
real append sink, SPU producer/drainer and host emulation remain linked intact.
The explicit ignored Rust target runs/validates the original before rolling.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def sha(path):
    if not path.is_file() or path.stat().st_size > 256 << 20:
        raise RuntimeError(f'not a bounded regular input: {path}')
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(65536), b''):
            h.update(block)
    return h.hexdigest()


def run(command, out, label, seconds):
    stdout, stderr = out / (label + '.stdout'), out / (label + '.stderr')
    with stdout.open('xb') as a, stderr.open('xb') as b:
        child = subprocess.Popen(command, stdout=a, stderr=b, start_new_session=True)
        start = time.monotonic()
        try:
            while child.poll() is None:
                if time.monotonic() - start > seconds or max(stdout.stat().st_size, stderr.stat().st_size) > 2 << 20:
                    raise RuntimeError(f'{label}: deadline/log budget exceeded; not behavioral evidence')
                time.sleep(.02)
            if max(stdout.stat().st_size, stderr.stat().st_size) > 2 << 20:
                raise RuntimeError(f'{label}: log budget exceeded')
            return child.returncode
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=10)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', choices=('address', 'undefined'))
    p.add_argument('--red-stubs', action='store_true')
    a = p.parse_args()
    if os.name != 'posix':
        raise RuntimeError('host C acceptance runs on Linux/WSL; no native Windows compiler path')
    out = a.build_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    lib = ROOT / 'sdk/libps3dkprof/src'
    include = out / 'include/ps3dk'; include.mkdir(parents=True)
    shutil.copyfile(ROOT / 'sdk/include/ps3dk/prof.h', include / 'prof.h')
    cc = shlex.split(a.cc)
    if not cc or shutil.which(cc[0]) is None:
        raise RuntimeError('compiler executable unavailable')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS',
             '-DPS3DK_PROF_WITH_SPU', '-DPS3DK_PROF_WITH_PROFILE', '-DPS3DK_PROF_ROLLING',
             '-I', str(include.parent)]
    for directory in ('core', 'rolling', 'pal', 'spu', 'rsx'):
        flags += ['-I', str(lib / directory)]
    if a.sanitize:
        flags += ['-fsanitize=' + ('address,undefined' if a.sanitize == 'address' else 'undefined'),
                  '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-ftls-model=local-exec']
    files = [lib / name for name in ('core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c',
             'pal/pal_host.c', 'rolling/roll_runtime.c', 'rolling/roll_store.c', 'rolling/roll_file.c',
             'rolling/roll_output.c', 'rolling/roll_ppu.c', 'rolling/roll_combined.c',
             'rolling/roll_retention_inspect.c', 'spu/psp_ppu.c', 'spu/psp_spu.c', 'spu/ext_runtime.c',
             'spu/psp_elf.c', 'spu/psp_host_emul.c', 'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c')]
    files += [HERE / 'runtime_spu_test.c']
    if a.red_stubs:
        files += [HERE / 'runtime_spu_red.c']
    wraps = ['malloc', 'calloc', 'realloc', 'free', 'posix_memalign', 'aligned_alloc',
             'ps3dk_prof__pal_open_excl', 'ps3dk_prof__pal_open_rw_excl',
             'ps3dk_prof__roll_combined_init', 'ps3dk_prof__roll_output_accept']
    exe = out / 'runtime-spu'
    command = cc + flags + list(map(str, files)) + ['-Wl,--wrap=' + name for name in wraps] + ['-o', str(exe)]
    inventory = sorted(set(p.resolve() for p in lib.rglob('*') if p.is_file() and p.suffix in ('.c', '.h')) |
                       {p.resolve() for p in HERE.glob('runtime_spu*')} |
                       {Path(__file__).resolve(), HERE.parent / 'ps3dk-prof-spu-int/i2_test.c',
                        ROOT / 'sdk/include/ps3dk/prof.h'})
    pins = {str(p): sha(p) for p in inventory}
    dependencies = set()
    for i, source in enumerate(files):
        label = f'deps-{i:02}'
        if run(cc + flags + ['-M', '-MT', 'input', str(source)], out, label, 60):
            raise RuntimeError(f'{label}: dependency discovery failed; not behavioral RED')
        text = (out / (label + '.stdout')).read_text().replace('\\\n', ' ')
        if not text.startswith('input:'):
            raise RuntimeError('unexpected dependency inventory format')
        dependencies.update(Path(x).resolve() for x in shlex.split(text.split(':', 1)[1]))
    if any(sha(Path(path)) != digest for path, digest in pins.items()):
        raise RuntimeError('source changed during dependency discovery')
    dependencies.add(Path(shutil.which(cc[0])).resolve())
    pins.update({str(path): sha(path) for path in dependencies})
    receipt = dict(command=command, red_stubs=a.red_stubs, inputs=pins,
                   command_sha256=hashlib.sha256(json.dumps(command).encode()).hexdigest())
    (out / 'inputs.json').write_text(json.dumps(receipt, indent=2) + '\n')
    rc = run(command, out, 'compile', 180)
    stable = all(sha(Path(path)) == digest for path, digest in pins.items())
    receipt.update(compile_rc=rc, inputs_unchanged=stable)
    if not rc:
        receipt['executable_sha256'] = sha(exe)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if rc or not stable:
        raise RuntimeError('compile/input gate failed; not behavioral RED')
    print(f'PS3DK_PROF_RUNTIME_SPU={exe}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

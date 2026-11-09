#!/usr/bin/env python3
"""Build/install only the opt-in librt_profwait.a, in both PPU data models.

Does not update default librt, CRT, compiler startfiles, or SDK headers. The
supplied SDK is read-only. Installation is optional and uses distinct files.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_pins(root, sdk):
    """Pin inputs before compilation and require an identical set afterwards."""
    paths={Path(__file__).resolve(),root/'scripts/build-runtime-lv2.sh'}
    for base in [root/'runtime/lv2/librt',root/'sdk/include',sdk/'ppu/include']:
        paths.update(h for h in base.rglob('*') if h.is_file())
    return {str(h):sha(h) for h in sorted(paths)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--install-prefix', type=Path)
    p.add_argument('--source-base', default=os.environ.get('PS3DK_PROF_WAIT_BASE',''))
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    cc = a.compiler.resolve(strict=True)
    sdk = a.sdk.resolve(strict=True)
    out = a.out.resolve()
    if out.exists():
        raise ValueError('output directory already exists; use a fresh build directory')
    tool = str(cc).removesuffix('.exe').removesuffix('gcc')
    suffix = '.exe' if cc.suffix.lower() == '.exe' else ''
    ar, readelf = Path(tool + 'ar' + suffix), Path(tool + 'readelf' + suffix)
    if not ar.is_file() or not readelf.is_file() or not (sdk / 'ppu/include').is_dir():
        raise ValueError('compiler tools or SDK include directory missing')
    sources = sorted((root / 'runtime/lv2/librt').glob('*.c'))
    if not sources or not any(s.name == 'pthread.c' for s in sources):
        raise ValueError('runtime source inventory is incomplete')
    out.mkdir(parents=True)
    env = dict(os.environ, PS3DK_NO_DEFAULT_INCLUDE='1')
    commands = []
    base=a.source_base
    if not base:
        base=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    if not re.fullmatch('[0-9a-f]{40}',base):
        raise ValueError('full source base commit is required')
    before=input_pins(root,sdk)
    compiler_pin=sha(cc)

    def run(name, argv):
        r = subprocess.run(list(map(str, argv)), env=env, capture_output=True, timeout=180)
        (out / (name + '.stdout')).write_bytes(r.stdout)
        (out / (name + '.stderr')).write_bytes(r.stderr)
        commands.append(dict(name=name, argv=list(map(str, argv)), rc=r.returncode))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if r.returncode:
            raise ValueError(f'{name} failed ({r.returncode}); see retained stderr')
        return r.stdout.decode()

    run('compiler-version', [cc, '-v'])
    manifest = dict(schema=1, kind='observed-pthread-runtime', base=base, compiler=str(cc),
                    compiler_sha256=compiler_pin, source_sha256={str(s.relative_to(root)): sha(s)
                    for s in [*sources, root / 'runtime/lv2/librt/prof_wait_adapter.h']}, abis={})
    for abi, af in [('ilp32', []), ('lp64', ['-mlp64'])]:
        dest = out / abi
        dest.mkdir()
        objects = []
        for src in sources:
            obj = dest / (src.stem + '.o')
            flags = [*af, '-mcpu=cell']
            if src.name == 'pthread.c':
                flags += ['-fexceptions', '-DPS3DK_PROF_WAIT_OBSERVE=1']
            flags += ['-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                      '-Werror=int-conversion', '-Wno-pointer-to-int-cast',
                      '-I', root / 'runtime/lv2/librt', '-I', root / 'sdk/include',
                      '-I', sdk / 'ppu/include']
            run(abi + '-' + src.stem, [cc, '-c', *flags, '-fstack-usage',
                                     '-MD', '-MF', obj.with_suffix('.d'), src, '-o', obj])
            # Make dependency escaping is retained verbatim in .d. This manifest
            # also pins the source and installed header trees independently.
            objects.append(obj)
        archive = dest / 'librt_profwait.a'
        run(abi + '-archive', [ar, 'rcsD', archive, *objects])
        symbols = run(abi + '-pthread-symbols', [readelf, '-sW', dest / 'pthread.o'])
        m = re.search(r'^\s*\d+:\s+[0-9a-fA-F]+\s+(\d+)\s+TLS\s+LOCAL\s+DEFAULT\s+\S+\s+pt_wait_observer_busy\s*$', symbols, re.M)
        if not m or int(m[1]) != 4:
            raise ValueError(abi + ': adapter TLS symbol size differs from the reviewed contract')
        for hook in ['ps3dk_prof__wait_begin', 'ps3dk_prof__wait_end',
                     '__ps3dk_prof_wait_thread_start', '__ps3dk_prof_wait_thread_exit']:
            if not re.search(r'\bWEAK\s+DEFAULT\s+UND\s+' + hook + r'\s*$', symbols, re.M):
                raise ValueError(abi + ': missing weak adapter hook ' + hook)
        manifest['abis'][abi] = dict(sha256=sha(archive), members=[o.name for o in objects],
                                     pthread_tls_symbol_bytes=4, optimization='default (O0)',
                                     observation_define='PS3DK_PROF_WAIT_OBSERVE=1 on pthread.c only')
        print(abi + ': ' + str(len(objects)) + ' members', flush=True)
    if input_pins(root,sdk)!=before or sha(cc)!=compiler_pin:
        raise ValueError('build inputs changed during compilation; archive must not be installed')
    manifest['input_sha256'] = before
    manifest['header_sha256'] = {name:digest for name,digest in before.items()
        if Path(name).is_relative_to(root/'sdk/include') or Path(name).is_relative_to(sdk/'ppu/include')}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if a.install_prefix:
        install = a.install_prefix.resolve()
        for abi, sub in [('ilp32', 'ppu/lib'), ('lp64', 'ppu/lib/lp64')]:
            dest = install / sub
            dest.mkdir(parents=True, exist_ok=True)
            archive = out / abi / 'librt_profwait.a'
            shutil.copyfile(archive, dest / archive.name)
            if sha(dest / archive.name) != manifest['abis'][abi]['sha256']:
                raise ValueError('installed observed archive verification failed')
            # An explicit opt-in search directory redirects the compiler's late
            # -lrt too. The normal librt.a and libpthread.a remain untouched.
            shim = dest / 'prof-waits'
            shim.mkdir(exist_ok=True)
            (shim / 'librt.a').write_text('/* Observed runtime selection; defaults remain separate. */\nINPUT(-lrt_profwait)\n')
            shutil.copyfile(out / 'manifest.json', shim / 'runtime-manifest.json')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        print('observed runtime: ' + str(e), file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Build the separate observed runtime and check its installation boundary.

Target compile/archive tests, not execution or a complete profiler link gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--sdk', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
builder = root / 'scripts/build-prof-wait-runtime.py'
checks = 0
commands = []


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(name, args):
    r = subprocess.run(list(map(str, args)), capture_output=True, timeout=300)
    (out / (name + '.stdout')).write_bytes(r.stdout)
    (out / (name + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name, argv=list(map(str, args)), rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2))
    return r


check(builder.is_file(), 'separate observed-runtime builder is missing')
cc = a.compiler.resolve()
ar = str(cc).removesuffix('gcc') + 'ar'
prefix = out / 'prefix'
for sub in ['ppu/lib', 'ppu/lib/lp64']:
    d = prefix / sub
    d.mkdir(parents=True)
    # Existing installed defaults must be completely outside this builder.
    for name in ['librt.a', 'libpthread.a', 'lv2-crt0.o']:
        (d / name).write_bytes(('preserved default ' + sub + '/' + name).encode())
defaults = {str(f.relative_to(prefix)): sha(f) for f in prefix.rglob('*') if f.is_file()}
sdk_before = {str(f): sha(f) for f in a.sdk.glob('ppu/lib/**/librt.a')}
args = [sys.executable, builder, '--compiler', cc, '--sdk', a.sdk,
        '--out', out / 'build', '--install-prefix', prefix]
r = run('build', args)
check(r.returncode == 0, 'observed build failed: ' + r.stderr.decode())
manifest = json.loads((out / 'build/manifest.json').read_text())
check(manifest['schema'] == 1, 'manifest schema')
check(manifest['compiler_sha256'] == sha(cc), 'compiler identity')
for abi, af, sub in [('ilp32', [], 'ppu/lib'), ('lp64', ['-mlp64'], 'ppu/lib/lp64')]:
    entry = manifest['abis'][abi]
    lib = out / 'build' / abi / 'librt_profwait.a'
    check(lib.read_bytes() == (prefix / sub / lib.name).read_bytes(), abi + ' installed bytes')
    check(entry['sha256'] == sha(lib), abi + ' manifest hash')
    members = run(abi + '-members', [ar, 't', lib])
    check(members.returncode == 0, abi + ' archive readable')
    names = members.stdout.decode().splitlines()
    check(names == entry['members'] and len(names) == len(set(names)), abi + ' exact member inventory')
    check('pthread.o' in names, abi + ' pthread member')
    check((prefix / sub / 'prof-waits/librt.a').read_text() ==
          '/* Observed runtime selection; defaults remain separate. */\nINPUT(-lrt_profwait)\n',
          abi + ' explicit-directory linker redirect')
    baseline = out / (abi + '-baseline')
    baseline.mkdir()
    for src in sorted((root / 'runtime/lv2/librt').glob('*.c')):
        # Reproduce the default build-runtime-lv2.sh compile flags; no observation
        # define. The tested builder adds only that define to pthread.c.
        obj = baseline / (src.stem + '.o')
        flags = [*af, '-mcpu=cell']
        if src.name == 'pthread.c':
            flags.append('-fexceptions')
        flags += ['-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                  '-Werror=int-conversion', '-Wno-pointer-to-int-cast',
                  '-I', root / 'runtime/lv2/librt', '-I', root / 'sdk/include',
                  '-I', a.sdk / 'ppu/include']
        r = run(abi + '-baseline-' + src.stem, [cc, '-c', *flags, src, '-o', obj])
        check(r.returncode == 0, abi + ' baseline compile ' + src.name)
        observed = out / 'build' / abi / obj.name
        check((obj.read_bytes() != observed.read_bytes()) == (src.name == 'pthread.c'),
              abi + ' only pthread changes: ' + src.name)
    check(entry['pthread_tls_symbol_bytes'] == 4, abi + ' adapter TLS symbol')
check(all(sha(prefix / n) == h for n, h in defaults.items()), 'installed default modified')
check(all(sha(Path(n)) == h for n, h in sdk_before.items()), 'input SDK default modified')
r = run('repeat-refused', args)
check(r.returncode != 0 and b'output directory already exists' in r.stderr,
      'existing build evidence must be preserved')
check(all(sha(prefix / n) == h for n, h in defaults.items()), 'refusal modified default')
(out / 'RESULT.json').write_text(json.dumps(dict(checks=checks, failed=0,
    default_files=defaults, input_sdk_files=sdk_before, scope=__doc__), indent=2))
print(f'{checks} checks, 0 failed', flush=True)

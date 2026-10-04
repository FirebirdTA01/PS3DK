#!/usr/bin/env python3
"""Exercise extension flag isolation through actual compilation, in both stages."""
import itertools
from pathlib import Path
import subprocess
import sys
import tempfile

NAMES = ('bom', 'declarator-types', 'static-parameters', 'glsl-functions',
         'glsl-types', 'utf16')


def check(compiler, root):
    listing = subprocess.run([compiler, '--list-extensions'], capture_output=True,
                             timeout=20)
    assert listing.returncode == 0, listing.stderr
    assert tuple(line.split('\t')[0] for line in listing.stdout.decode().splitlines()) == NAMES
    calls = 0

    def run(text, profile, flags):
        nonlocal calls
        calls += 1
        src = root / f'{calls}.cg'
        dst = root / f'{calls}.bin'
        src.write_bytes(text)
        p = subprocess.run([compiler, '-p', profile,
                            *(f'--extension={f}' for f in flags),
                            '--emit-container', str(dst), str(src)],
                           capture_output=True, timeout=20)
        diagnostic = p.stdout + p.stderr
        assert p.returncode in (0, 1), (flags, p.returncode, diagnostic)
        if p.returncode == 0:
            assert dst.is_file() and dst.stat().st_size > 32, (flags, diagnostic)
            return dst.read_bytes(), diagnostic
        assert not dst.exists(), (flags, 'failed compile left artifact')
        assert b'error:' in diagnostic, (flags, diagnostic)
        return None, diagnostic

    for profile, semantic in (('sce_fp_rsx', 'COLOR'), ('sce_vp_rsx', 'POSITION')):
        plain = f'float4 main(float4 t:TEXCOORD0):{semantic}{{return t;}}'
        canonical, _ = run(plain.encode(), profile, ())
        assert canonical is not None
        for flag in NAMES:
            result, _ = run(plain.encode(), profile, (flag,))
            assert result == canonical, ('plain changed', flag, profile)
        bodies = {
            'bom': b'\xef\xbb\xbf' + plain.encode(),
            'utf16': b'\xff\xfe' + plain.encode('utf-16le'),
            'static-parameters': ('float4 helper(static float4 x){return x;}\n' +
                plain.replace('return t;', 'return helper(t);')).encode(),
            'glsl-functions': plain.replace('return t;', 'return mix(t,t,0.5);').encode(),
            'glsl-types': plain.replace('return t;', 'vec4 x=t; return x;').encode(),
        }
        for own, body in bodies.items():
            for flags in [(), *((x,) for x in NAMES), NAMES]:
                result, diagnostic = run(body, profile, flags)
                if own in flags:
                    assert result is not None, (profile, own, flags, diagnostic)
                    # The encoding and static-storage forms are identity shaders.
                    if own in ('bom', 'utf16', 'static-parameters', 'glsl-types'):
                        assert result == canonical, (profile, own, flags, 'changed identity shader')
                else:
                    assert result is None, (profile, own, flags, 'unrelated flag admitted construct')
                    assert f'--extension={own}'.encode() in diagnostic, (profile, own, diagnostic)
        # Encoding admission and language admission must both be enabled.
        mixed = b'\xff\xfe' + bodies['static-parameters'].decode().encode('utf-16le')
        for count in range(3):
            for flags in itertools.combinations(('utf16', 'static-parameters'), count):
                result, diagnostic = run(mixed, profile, flags)
                assert (result is not None) == (count == 2), (profile, flags, diagnostic)
                if count == 2:
                    assert result == canonical
    assert calls == 102, calls
    print(f'PASS: extension registry isolation ({calls} compiles, 6 distinct flags, both profiles)')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='rsxcg-extension-registry-') as work:
        check(str(Path(sys.argv[1]).resolve()), Path(work))

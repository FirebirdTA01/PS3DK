"""Authored refusal diagnostics: exact exit 1, no output, named first cause.

These are existing lowering gaps, not acceptance tests. Keep their diagnostics
useful to callers that display one line and to census tools that bucket it.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path


CASES = [
    ('call_fp', 'fp', 'float4 main(float4 c:TEXCOORD0):COLOR {return fmod(c,2);}',
     r'unsupported IR op call.*@fmod'),
    ('call_vp', 'vp', 'void main(float4 p:POSITION,out float4 o:POSITION,out float4 c:COLOR) {o=p;c=fmod(p,2);}',
     r'unsupported IR op call.*@fmod'),
    # A semantic-less struct member now takes an implicit TEXCOORD (measured;
    # value-checked by implicit-varying); 'semantic' keeps this message's shape.
    ('semantic', 'fp', 'struct Input {float4 shade:TEXUNIT0;}; float4 main(Input data):COLOR {return data.shade;}',
     r"unsupported input semantic.*TEXUNIT0.*entry 'main'.*input 'data.shade'.*vec4"),
    ('sample_lod', 'fp', 'float4 main(float4 uv:TEXCOORD0,uniform sampler2D s):COLOR {return tex2Dlod(s,uv);}',
     r'unsupported IR op samplelod'),
]


def valid(rc, has_output, log, pattern):
    lines = [line.strip() for line in log.splitlines() if line.strip()]
    return (rc == 1 and not has_output and bool(lines)
            and lines[0].startswith('nv40-general:')
            and re.search(pattern, lines[0]) is not None)


def main():
    compiler = sys.argv[1]
    # A crash, stale output or a name mentioned only after the generic banner
    # cannot stand in for a useful first diagnostic.
    good = 'nv40-general: unsupported IR op call @fmod'
    controls = [(124, False, good), (1, True, good),
                (1, False, 'nv40-general: refusing to emit - the program did not lower completely\n' + good)]
    for args in controls:
        assert not valid(*args, r'unsupported IR op call.*@fmod')
    assert valid(1, False, good, r'unsupported IR op call.*@fmod')
    passed = len(controls) + 1
    failed = 0
    with tempfile.TemporaryDirectory(prefix='rsxcg-diagnostics-') as tmp:
        for name, profile, source, pattern in CASES:
            src = Path(tmp) / (name + '.cg')
            dst = Path(tmp) / (name + '.bin')
            src.write_text(source, encoding='utf-8')
            result = subprocess.run([compiler, '-p', 'sce_' + profile + '_rsx',
                                     '--emit-container', str(dst), str(src)],
                                    capture_output=True, text=True, timeout=30)
            # stderr is the diagnostic channel; stdout's IR summary is not it.
            ok = valid(result.returncode, dst.exists(), result.stderr, pattern)
            if ok:
                passed += 1
                print('PASS:', name)
            else:
                failed += 1
                print('FAIL:', name, 'exit=', result.returncode,
                      'output=', dst.exists(), '\n' + result.stderr)
    print(f'backend-diagnostics: {passed + failed} tests, {passed} pass, {failed} fail')
    return int(failed != 0)


if __name__ == '__main__':
    sys.exit(main())

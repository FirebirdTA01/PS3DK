"""Empty statements preserve surrounding control flow and condition effects.

Authored oracle probes compile each finite pair to identical containers on FP
and VP. Infinite/dynamic loops remain named gaps; they are never executed.
"""
import itertools
import subprocess
import sys
import tempfile
from pathlib import Path
import fp_eval

PAIRS = {
    'standalone': ('; return c;', 'return c;'),
    'block_tail': ('{ c.xy=c.yx; }; return c;', '{ c.xy=c.yx; } return c;'),
    'if_empty': ('if (c.x>0) ; return c;', 'if (c.x>0) {} return c;'),
    'if_else_empty': ('if (c.x>0) ; else ; return c;', 'if (c.x>0) {} else {} return c;'),
    'if_side_effect': ('float x=c.x; if (x++>0) ; return float4(x,c.yzw);',
                       'float x=c.x; if (x++>0) {} return float4(x,c.yzw);'),
    'finite_for': ('for (int i=0;i<3;++i) ; return c;', 'return c;'),
}
REFUSALS = {
    'infinite_for': ('for (;;) ; return c;', 'back-edge'),
    'dynamic_while': ('while (c.x>0) ; return c;', 'back-edge'),
    'case_outside': ('case 1: return c;', None),
    'default_outside': ('default: return c;', None),
}


def accepted(rc, blob):
    return rc == 0 and bool(blob)


def refused(rc, exists, stderr, message):
    return rc == 1 and not exists and (message is None or message in stderr)


def main():
    compiler = sys.argv[1]
    assert fp_eval.self_test()
    # Neither a stale output nor a timeout/crash stands in for refusal.
    assert not refused(124, False, 'back-edge', 'back-edge')
    assert not refused(1, True, 'back-edge', 'back-edge')
    assert not accepted(0, b'')
    assert not accepted(1, b'stale')
    passed, failed, debts = 4, 0, 0
    with tempfile.TemporaryDirectory(prefix='rsxcg-empty-stmt-') as tmp:
        directory = Path(tmp)

        def compile_body(profile, name, body):
            src = directory / (profile + '-' + name + '.cg')
            dst = src.with_suffix('.bin')
            src.write_text('float4 main(float4 c:TEXCOORD0):' +
                           ('COLOR' if profile == 'fp' else 'POSITION') + '{' + body + '}\n', encoding='utf-8')
            result = subprocess.run([compiler, '-p', 'sce_' + profile + '_rsx',
                                     '--emit-container', str(dst), str(src)],
                                    capture_output=True, text=True, timeout=30)
            return result, dst

        for profile in ('fp', 'vp'):
            for name, (body, twin) in PAIRS.items():
                a, ap = compile_body(profile, name, body)
                b, bp = compile_body(profile, name + '-control', twin)
                ab = ap.read_bytes() if ap.exists() else b''
                bb = bp.read_bytes() if bp.exists() else b''
                ok = accepted(a.returncode, ab) and accepted(b.returncode, bb) and ab == bb
                if ok and profile == 'fp':
                    for vector in itertools.product([-.5, 0., .5, 1.], repeat=4):
                        c = list(vector)
                        want = ([c[1], c[0], c[2], c[3]] if name == 'block_tail' else
                                [c[0] + 1., *c[1:]] if name == 'if_side_effect' else c)
                        ok = ok and fp_eval.evaluate(ab, {'TEX0': c}) == want
                passed += int(ok)
                failed += int(not ok)
                print('PASS' if ok else 'FAIL', profile, name)
                if not ok:
                    print(a.returncode, a.stderr, b.returncode, b.stderr)
            for name, (body, message) in REFUSALS.items():
                result, dst = compile_body(profile, name, body)
                ok = refused(result.returncode, dst.exists(), result.stderr, message)
                passed += int(ok)
                failed += int(not ok)
                print('PASS' if ok else 'FAIL', profile, name)
                if not ok:
                    print(result.returncode, result.stderr)
            # Existing CFG limitation: even while(false){} retains a back-edge.
            # The oracle accepts both and folds them to passthrough. Keep this
            # visible as debt; handling Empty must not erase enclosing loops.
            outcomes = [compile_body(profile, 'false_while_' + suffix, body)
                        for suffix, body in [('empty', 'while(false) ; return c;'),
                                             ('block', 'while(false) {} return c;')]]
            if all(refused(r.returncode, p.exists(), r.stderr, 'back-edge') for r, p in outcomes):
                debts += 1
                print('KNOWN-DEBT', profile, 'false_while: existing CFG loop refusal (control-flow plan)')
            else:
                failed += 1
                print('FAIL', profile, 'false_while refusal changed')
    print(f'empty-statement: {passed + failed + debts} tests, {passed} pass, {failed} fail, {debts} known-debt')
    return int(failed != 0)


if __name__ == '__main__':
    sys.exit(main())

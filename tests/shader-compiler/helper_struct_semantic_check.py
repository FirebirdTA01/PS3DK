"""Helper struct annotations do not bind shader inputs.

The selected entry still refuses unsupported aggregate annotations. Numeric
helper cases compare the complete container to an unannotated control, and
independently check values in both stages. No entry binding support is implied.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[-.5, .25, 1.5, 2.], [0., -.5, .25, 1.], [.25, 1.5, -.5, .5]]
ANNOTATIONS = ('TEXUNIT1', 'TEXCOORD3', 'POSITION', 'UNRECOGNIZED')


def check(compiler, work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    errors = []
    twins = values = refusals = 0

    def compile_case(name, text, stage, entry='selected'):
        src, dst = work / (name + '.cg'), work / (name + '.bin')
        src.write_text(text)
        dst.unlink(missing_ok=True)
        p = subprocess.run([compiler, '-p', 'sce_' + stage + '_rsx', '-e', entry,
                            '--emit-container', str(dst), str(src)],
                           capture_output=True, text=True, timeout=30)
        log = p.stdout + p.stderr
        (work / (name + '.log')).write_text(log)
        return p.returncode, dst.read_bytes() if dst.exists() else b'', log

    for stage in ('fp', 'vp'):
        sin, sout = ('TEXCOORD0', 'COLOR') if stage == 'fp' else ('POSITION', 'POSITION')
        for annotation in ANNOTATIONS:
            for mode in ('dormant', 'called', 'main_helper'):
                helper = 'main' if mode == 'main_helper' else 'helper'
                # FP whole-struct member reads in a called helper remain a
                # separate backend limit. Keep its unused aggregate argument
                # here; VP additionally exercises the member value itself.
                expr = 'q.wzyx*2' if stage == 'fp' else 'p.c.wzyx*2'
                body = 'return t;' if mode == 'dormant' else 'S s;s.c=t;return %s(s,t);' % helper
                template = ('struct S{float4 c;}; float4 %s(S pANNOTATION,float4 q){return %s;}'
                            'float4 selected(float4 t:%s):%s{%s}' % (helper, expr, sin, sout, body))
                blobs = []
                for kind, sem in (('annotated', ':' + annotation), ('plain', '')):
                    name = '_'.join((stage, mode, annotation, kind))
                    rc, blob, log = compile_case(name, template.replace('ANNOTATION', sem), stage)
                    if rc or not blob:
                        errors.append('%s: expected container, rc=%s: %s' % (name, rc, log.strip()))
                        continue
                    blobs.append(blob)
                    for t in INPUTS:
                        want = t if mode == 'dormant' else [x * 2 for x in t[::-1]]
                        got = (fp_eval.evaluate(blob, {'TEX0': t}) if stage == 'fp' else
                               vp.evaluate(blob, {}, inputs={0: t}, binary32=True, predication=True).get(0))
                        values += 1
                        if got != want:
                            errors.append('%s: input=%s got=%s want=%s' % (name, t, got, want))
                if len(blobs) == 2:
                    twins += 1
                    if blobs[0] != blobs[1]:
                        errors.append('%s/%s/%s: annotated container differs' % (stage, mode, annotation))

        # Named entry and main selected explicitly both retain the same refusal.
        # A helper-only relaxation must not silently invent aggregate bindings.
        for entry in ('selected', 'main'):
            text = 'struct S{float4 c;}; float4 %s(S p:TEXCOORD0):%s{return p.c;}' % (entry, sout)
            rc, blob, log = compile_case(stage + '_entry_' + entry, text, stage, entry)
            if rc != 1 or blob or 'semantics on struct-typed parameters are not supported' not in log or '--extension=' in log:
                errors.append('%s/%s: expected specific entry refusal, rc=%s: %s' % (stage, entry, rc, log))
            else:
                refusals += 1
    for error in errors:
        print('FAIL:', error)
    print('helper-struct-semantic: %s twins=%d values=%d refusals=%d' %
          ('FAIL' if errors else 'PASS', twins, values, refusals))
    return int(bool(errors))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--work', type=Path)
    args = ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return check(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix='helper-struct-semantic-') as tmp:
        return check(args.compiler, Path(tmp))


if __name__ == '__main__':
    raise SystemExit(main())

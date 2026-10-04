"""Folded matrix splats retain every row and column at backend consumers.

Use finite nonzero dyadic inputs for the existing x/x simplification; this
test changes neither that optimization nor its exceptional-value policy.
Expected values are independent of the explicit matrix constructor control.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[.5, 1., 2., 4.], [-.5, -1., -2., -4.], [2., 4., .5, 1.]]


def cases():
    for rows in range(1, 5):
        for cols in range(1, 5):
            ty = f'float{rows}x{cols}'
            cells = [f't[{i % 4}]' for i in range(rows * cols)]
            init = f'{ty} m={ty}(' + ','.join(cells) + ');'
            for row in range(rows):
                components = [f'n[{row}]' if cols == 1 else f'n[{row}][{c}]' for c in range(cols)]
                # Nonzero padding makes every output lane explicit in the
                # reference too; its unwritten-zero lanes are not modelled.
                result = 'return float4(' + ','.join(components + ['2.0'] * (4-cols)) + ');'
                for name, op, value in [('one', '/', 1.), ('zero', '-', 0.)]:
                    folded = init + f'{ty} n=m+(m{op}m);' + result
                    explicit = init + f'{ty} n=m+{ty}(' + ','.join([str(value)]*(rows*cols)) + ');' + result
                    def want(t, row=row, cols=cols, value=value):
                        return [t[(row*cols+c) % 4]+value for c in range(cols)] + [2.]*(4-cols)
                    yield f'{rows}x{cols}-row{row}-{name}', folded, explicit, want


def source(body, stage):
    si, so = ('TEXCOORD0','COLOR') if stage == 'fp' else ('POSITION','POSITION')
    return f'float4 main(float4 t:{si}):{so}{{{body}}}\n'


def check(compiler, work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    failures=[]; pairs=values=refusals=0
    for name, body, control, expected in cases():
        for stage in ['fp','vp']:
            passed=0
            for mode, text in [('folded',body), ('explicit',control)]:
                src=work/f'{name}-{stage}-{mode}.cg'; dst=src.with_suffix('.bin')
                src.write_text(source(text,stage)); dst.unlink(missing_ok=True)
                run=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.log').write_text(run.stdout+run.stderr)
                if run.returncode or not dst.exists():
                    failures.append(f'{name}/{stage}/{mode}: refused {run.returncode}'); continue
                passed+=1
                for t in INPUTS:
                    blob=dst.read_bytes()
                    got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                    values+=1
                    if got!=expected(t):failures.append(f'{name}/{stage}/{mode}: {t}: {got} != {expected(t)}')
            pairs+=passed==2
    for stage in ['fp','vp']:
        for name,ty,op,diagnostic in [
            ('fixed_sub_debt','fixed','-', "unknown type name 'fixed2x2'"),
            ('fixed_div_debt','fixed','/', "unknown type name 'fixed2x2'"),
            ('half_splat_debt','half','/', 'matrix arithmetic is not yet lowered')]:
            # Fixed matrices are currently refused by the parser, not an
            # inferred Float32 IR provenance guard. Revisit when that surface
            # is implemented: fixed scalar/vector precision is separate debt.
            body=f'{ty}2x2 m={ty}2x2(t);{ty}2x2 n=m+(m{op}m);return float4(n[0],n[1]);'
            src=work/f'{name}-{stage}.cg';dst=src.with_suffix('.bin')
            src.write_text(source(body,stage));dst.unlink(missing_ok=True)
            run=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            src.with_suffix('.log').write_text(run.stdout+run.stderr)
            if run.returncode==1 and not dst.exists() and diagnostic in run.stderr and '--extension=' not in run.stderr:
                refusals+=1
            else:failures.append(f'{name}/{stage}: expected pinned refusal without artifact or extension hint')
    if not failures and (pairs!=160 or values!=960 or refusals!=6):
        failures.append(f'incomplete coverage: pairs={pairs} values={values}')
    for failure in failures[:16]:print('FAIL:',failure)
    print(f'matrix-folded-constant: {"FAIL" if failures else "PASS"} pairs={pairs} values={values} refusals={refusals} failures={len(failures)}')
    return int(bool(failures))


if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('compiler'); ap.add_argument('--work',type=Path); args=ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True); raise SystemExit(check(args.compiler,args.work))
    with tempfile.TemporaryDirectory(prefix='matrix-folded-constant-') as tmp:
        raise SystemExit(check(args.compiler,Path(tmp)))

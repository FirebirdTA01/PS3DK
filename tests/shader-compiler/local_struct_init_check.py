"""Authored local-struct initializer witnesses, measured on sce-cgc 475.

Both profiles use the same sources, with POSITION replacing COLOR on VP.
The reference accepts all cases except the five malformed field lists below.
Unsupported aggregate contexts remain strict named debts, never value passes.
"""
import argparse
import itertools
import json
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval
from uniform_container_check import Container
from uniform_struct_param_check import with_uniforms
from vp_pow_vector_check import evaluate as evaluate_vp

CASES = json.loads(Path(__file__).with_name('local_struct_init_cases.json').read_text())
REFUSED = {'few', 'many', 'wrong_struct', 'numeric_splat', 'numeric_narrow',
           'uint_field'}  # The reference rejects the bare uint spelling (C0000).
DEBTS = {'array_braces', 'array_member', 'sampler_field',
         'constructor_as_argument', 'constructor_member',
         'global_const_nested', 'uniform_default_nested',
         'short_field', 'bool_field'}
VP_DEBTS = {'int_field': 'VP float-to-int lowering deferred',
            'numeric_half': 'half precision is fragment-only',
            'implicit_member': 'VP int-to-float lowering deferred'}


def expected(name, t, u):
    if name.startswith('unreachable_'):
        return list(t)
    if name == 'argument_once':
        return [t[0], t[0] + 1, t[0] + 2, 1.]
    if name == 'copy_independence':
        return [t[0] * t[2], t[1] * t[2], t[2], t[3]]
    if name == 'global_side_effect':
        return [t[0] * t[2], t[1] * t[2], 1., 1.]
    if name in ('implicit_member', 'typedef_field', 'typedef_struct'):
        return [t[0], t[1], 0., 1.]
    # fixed members convert as a fixed declaration does; these inputs are
    # exact in fixed, and the reference's containers give these values.
    if name == 'fixed_field':
        return [t[0], 0., 0., 1.]
    if name == 'fixed_source':
        return [t[0], t[1], 0., 1.]
    if name == 'int_field':
        return [float(math.trunc(t[0])), 0., 0., 1.]
    if name == 'matrix_field':
        return [t[0] + 2 * t[1], t[2] + 2 * t[3], 0., 1.]
    if name == 'nested_uniform':
        return [u[0] * t[2], u[1] * t[2], 0., 1.]
    return [t[0] * t[2], t[1] * t[2], 0., 1.]


def judge(name, blob, profile):
    vertex = profile == 'sce_vp_rsx'
    count = 0
    for u in ((.5, 2., 0., 0.), (2., -.5, 0., 0.)):
        patched = blob
        if name == 'nested_uniform':
            container = Container(blob)
            records = [r for r in container.records if r['name'] == 'u.uv']
            assert len(records) == 1 and records[0]['referenced'] == 1, records
            if not vertex:
                patched = with_uniforms(blob, container, {'u.uv': u})
        for t in itertools.product((-.5, 0., .25, 1.5), repeat=4):
            # Distinct unused input catches incorrect read-set allocation.
            inputs = {'TEX0': list(t), 'TEX1': [8., 4., 2., 1.]}
            got = (evaluate_vp(patched, {'u.uv': u},
                               {8: list(t), 9: inputs['TEX1']}, binary32=True).get(0)
                   if vertex else fp_eval.evaluate(patched, inputs))
            want = expected(name, t, u)
            assert got == want, (name, profile, t, u, got, want)
            count += 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--keep', type=Path)
    args = ap.parse_args()
    assert fp_eval.self_test()
    failures = []
    debts = []
    values = 0
    temporary = tempfile.TemporaryDirectory(prefix='local-struct-init-')
    work = args.keep or Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    for profile in ('sce_fp_rsx', 'sce_vp_rsx'):
        for name, source in CASES.items():
            key = profile + '-' + name
            source = source.replace(':COLOR', ':POSITION') if profile == 'sce_vp_rsx' else source
            src = work / (key + '.cg')
            dst = work / (key + '.bin')
            # Each attempt must start without an output; stale files never pass.
            if dst.exists():
                raise RuntimeError('output already exists: ' + str(dst))
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', profile, '--emit-container',
                                  str(dst), str(src)], capture_output=True, text=True, timeout=30)
            (work / (key + '.log')).write_text(run.stdout + run.stderr)
            if name in REFUSED | DEBTS:
                if run.returncode != 1 or dst.exists() or 'local-struct-initializer' not in run.stderr:
                    failures.append(key + ': expected exit 1, no container, named refusal: ' + run.stderr)
                elif name in DEBTS:
                    debts.append(key)
                continue
            # Unimplemented VP conversions and two-column matvec remain
            # explicit; acceptance must fail this debt and become a value row.
            if profile == 'sce_vp_rsx' and name in VP_DEBTS:
                reason = VP_DEBTS[name]
                if run.returncode != 1 or dst.exists() or reason not in run.stderr:
                    failures.append(key + ': expected existing named VP gap: ' + run.stderr)
                else:
                    debts.append(key)
                continue
            if run.returncode != 0 or not dst.exists():
                failures.append(key + ': unexpected refusal: ' + run.stderr)
                continue
            try:
                values += judge(name, dst.read_bytes(), profile)
            except (AssertionError, ValueError, RuntimeError) as exc:
                failures.append(key + ': ' + str(exc))
    for failure in failures:
        print('FAIL:', failure)
    for debt in debts:
        print('KNOWN-DEBT:', debt, '(reference accepts; not value-judged)')
    print('local-struct-init:', 'FAIL' if failures else 'PASS',
          'failures=', len(failures), 'values=', values, 'debts=', len(debts))
    temporary.cleanup()
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())

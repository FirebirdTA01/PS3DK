"""Compile real containers and falsify the reusable checker by byte mutation."""
import pathlib
import collections
import json
import struct
import subprocess
import sys
import tempfile

from uniform_container_check import check_container
from vp_binding_check import evaluate_bindings
from uniform_container_census import allowance_delta, issue_key


def records(blob):
    count, table = struct.unpack_from('>II', blob, 12)
    for i in range(count):
        offset = table + 48 * i
        row = struct.unpack_from('>12I', blob, offset)
        name = blob[row[4]:blob.index(0, row[4])].decode()
        yield name, offset, row


def codes(blob):
    return {x['code'] for x in check_container(blob)['issues']}


def main(compiler):
    with tempfile.TemporaryDirectory(prefix='ps3dk-uniform-crosscheck-') as tmp:
        work = pathlib.Path(tmp)

        def compile(profile, source):
            path, output = work / 'probe.cg', work / 'probe.bin'
            path.write_text(source)
            run = subprocess.run([compiler, '-p', profile, '--emit-container', str(output), str(path)], capture_output=True, timeout=20)
            assert run.returncode == 0, (run.stdout + run.stderr).decode(errors='replace')
            return output.read_bytes()

        fp = compile('sce_fp_rsx', 'uniform float4 u=float4(1,2,3,4); float4 main(float4 p:TEXCOORD0):COLOR {return u*p + u*p.wzyx;}')
        assert not codes(fp), check_container(fp)
        assert not check_container(fp, expected_profile=7004)['issues']
        assert any(i['code'] == 'container_profile_mismatch' for i in
                   check_container(fp, expected_profile=7003)['issues']), 'a wrong-profile container must fail'
        row = next(r for n, _, r in records(fp) if n == 'u')
        n = struct.unpack_from('>I', fp, row[6])[0]
        offsets = struct.unpack_from('>' + 'I' * n, fp, row[6] + 4)
        assert n >= 2, 'second-occurrence mutation must exercise a repeated uniform'
        ucode = struct.unpack_from('>I', fp, 28)[0]
        changed = bytearray(fp)
        struct.pack_into('>I', changed, ucode + offsets[-1], 0)
        assert 'default_inline_mismatch' in codes(changed)
        for bits in (0x80000000, 0x7fc00001):
            changed = bytearray(fp)
            struct.pack_into('>I', changed, ucode + offsets[-1], ((bits << 16) | (bits >> 16)) & 0xffffffff)
            assert 'default_inline_mismatch' in codes(changed)
        zero = bytearray(fp)
        struct.pack_into('>I', zero, row[5], 0)
        for offset in offsets:
            struct.pack_into('>I', zero, ucode + offset, 0)
        assert not codes(zero), check_container(zero)
        struct.pack_into('>I', zero, ucode + offsets[-1], 0x00008000)  # -0 after halfword unswap
        assert 'default_inline_mismatch' in codes(zero), 'raw comparison must distinguish +0 and -0'
        changed = bytearray(fp)
        struct.pack_into('>I', changed, row[6] + 4, 0)  # instruction, not inline data
        assert 'invalid_embedded_offset' in codes(changed)
        changed = bytearray(fp)
        struct.pack_into('>I', changed, row[6] + 8, offsets[0])
        assert 'duplicate_embedded_offset' in codes(changed)
        assert 'malformed_container' in codes(fp[:31])
        missing = bytearray(fp)
        _, uniform_pos, _ = next(r for r in records(fp) if r[0] == 'u')
        struct.pack_into('>I', missing, uniform_pos + 24, 0)
        assert 'referenced_default_without_inline_use' in codes(missing), 'lost relocation list must not pass'
        # Reference unused-with-default records have isReferenced=0 and no list.
        struct.pack_into('>I', missing, uniform_pos + 40, 0)
        assert not codes(missing), check_container(missing)
        empty = struct.pack('>8I', 7004, 0, 32, 0, 32, 0, 0, 32)
        assert 'empty_ucode' in codes(empty), 'absence of instructions is unresolved coverage'
        changed = bytearray(fp)
        struct.pack_into('>I', changed, uniform_pos + 8, 4101)
        struct.pack_into('>I', changed, row[6] + 4, 0)
        assert 'invalid_embedded_offset' in codes(changed), 'variability must not hide a bad relocation'
        nodefault = compile('sce_fp_rsx', 'uniform float4 u; float4 main(float4 p:TEXCOORD0):COLOR {return p*u;}')
        assert not codes(nodefault), check_container(nodefault)
        r = next(r for n, _, r in records(nodefault) if n == 'u')
        assert r[5] == 0 and r[6] != 0, 'defaultless uniforms still have patch locations'
        matrix = compile('sce_fp_rsx', 'uniform float3x3 m; float4 main(float3 p:TEXCOORD0):COLOR {return float4(mul(m,p),1);}')
        assert not codes(matrix), check_container(matrix)
        _, parent_pos, _ = next(r for r in records(matrix) if r[0] == 'm')
        _, row_pos, row_data = next(r for r in records(matrix) if r[0] == 'm[2]')
        changed = bytearray(matrix)
        struct.pack_into('>I', changed, parent_pos + 24, row_data[6])
        struct.pack_into('>I', changed, row_pos + 24, 0)
        struct.pack_into('>I', changed, row_data[6] + 4, 0)
        assert 'invalid_embedded_offset' in codes(changed), 'parent classification must not hide a bad relocation'

        vp = compile('sce_vp_rsx', 'uniform float4 u:register(C9); float4 main(float4 p:POSITION):POSITION {return p*u;}')
        assert not codes(vp), check_container(vp)
        _, pos, _ = next(r for r in records(vp) if r[0] == 'u')
        changed = bytearray(vp)
        struct.pack_into('>I', changed, pos + 12, 10)
        assert {'unread_uniform_register', 'undeclared_constant_read'} <= codes(changed)
        report = check_container(changed)
        source_key = {'source': 'selftest.cg', 'profile': 'sce_vp_rsx'}
        findings = collections.Counter(issue_key(source_key, i) for i in report['issues'])
        allowances = [dict(source_key, **{k: v for k, v in i.items() if k not in ('detail', 'status')},
                           card='t_25fa9e31', reason='self-test only', count=1) for i in report['issues']]
        assert not allowance_delta(findings, allowances)
        struct.pack_into('>I', changed, pos + 12, 11)
        more = collections.Counter(issue_key(source_key, i) for i in check_container(changed)['issues'])
        assert allowance_delta(more, allowances), 'allowance must not absorb a different register'
        assert allowance_delta(collections.Counter(), allowances), 'stale allowance must fail'
        baseline = json.loads(pathlib.Path(__file__).with_name('uniform_container_allowlist.json').read_text())
        indirect = next(e for e in baseline if e['code'] == 'indirect_constant_read')
        original = collections.Counter({issue_key(indirect, indirect): indirect['count']})
        assert not allowance_delta(original, [indirect])
        for field in ('offset', 'slot'):
            changed_entry = dict(indirect, **{field: indirect[field] + 1})
            added = original.copy()
            added[issue_key(changed_entry, changed_entry)] += 1
            delta = allowance_delta(added, [indirect])
            assert len(delta) == 1 and delta[0]['actual'] == 1 and delta[0]['allowed'] == 0, (
                'an additional indirect read differing only in %s must be new' % field, delta)
        extra_occurrence = original.copy()
        extra_occurrence[issue_key(indirect, indirect)] += 1
        assert allowance_delta(extra_occurrence, [indirect]), 'same-location count growth must fail'
        alias = compile('sce_vp_rsx', 'uniform float4 u:register(C9); uniform float4 v:register(C9); float4 main(float4 p:POSITION):POSITION {return p*u+v;}')
        assert not codes(alias), check_container(alias)
        scalar = compile('sce_vp_rsx', 'uniform float u:register(C9); float4 main(float4 p:POSITION):POSITION {return p/u;}')
        assert not codes(scalar), check_container(scalar)
        assert any(r['slot'] == 2 and r['register'] == 9 for r in check_container(scalar)['reads'])
        unused = compile('sce_vp_rsx', 'uniform float4 u:register(C9); float4 main(float4 p:POSITION):POSITION {return p;}')
        assert not codes(unused), check_container(unused)
        rowonly = compile('sce_vp_rsx', 'uniform float3x3 m; float4 main(float4 p:POSITION):POSITION {return float4(m[2],p.w);}')
        assert not codes(rowonly), check_container(rowonly)
        assert sum(r['code'] == 'aggregate_row_liveness' for r in check_container(rowonly)['checks']) == 2
        changed = bytearray(vp)
        at = struct.unpack_from('>I', vp, 28)[0] + 4
        word = struct.unpack_from('>I', vp, at)[0]
        struct.pack_into('>I', changed, at, word | (31 << 27))
        assert 'unknown_vp_encoding' in codes(changed)
        bindings = compile('sce_vp_rsx', 'uniform float4x4 N:register(C9); uniform float4x4 M[2]; float4 main(float4 p:POSITION):POSITION {return mul(N,p)+mul(M[0],p.wzyx)+mul(M[1],p.yxwz);}')
        assert not codes(bindings), check_container(bindings)
        constants = {r: [float((r+3)*(j+2)**2 + ((r*j)%7)*3)/8 for j in range(4)]
                     for r in list(range(9, 13)) + list(range(256, 264))}
        value, _ = evaluate_bindings(bindings, constants, {'IN0': [1., 2., 3., 4.]})
        expected = [sum(sum(constants[base+i][j]*vector[j] for j in range(4))
                        for base, vector in ((9, [1,2,3,4]), (256, [4,3,2,1]), (260, [2,1,4,3]))) for i in range(4)]
        assert value == expected, (value, expected)
        # The structural check CANNOT identify this permutation of declarations.
        # The independent asymmetric witness must catch what membership cannot.
        swapped = bytearray(bindings)
        h = struct.unpack_from('>8I', bindings)
        permutation = dict(zip(range(9, 13), range(256, 260)))
        permutation.update(zip(range(256, 260), range(9, 13)))
        for at in range(h[7], h[7] + h[6], 16):
            word = struct.unpack_from('>I', bindings, at + 4)[0]
            reg = (word >> 12) & 511
            if reg in permutation:
                struct.pack_into('>I', swapped, at + 4, (word & ~(511 << 12)) | (permutation[reg] << 12))
        assert not codes(swapped), 'membership cannot prove a declaration identity'
        wrong, _ = evaluate_bindings(swapped, constants, {'IN0': [1., 2., 3., 4.]})
        assert wrong != expected, 'asymmetric identity witness must distinguish a register-set-preserving swap'
    print('uniform-container-check: PASS (real containers and shared-callable mutations)')


if __name__ == '__main__':
    main(sys.argv[1])

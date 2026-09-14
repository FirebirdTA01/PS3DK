"""Cross-check NV40 uniform metadata against encoded operands (t_25fa9e31).

This checks structural agreement, not declaration identity: swapping two live
uniforms while preserving the register set requires independent value witnesses.
FP C-register bindings are metadata; VP defaults are uploaded by the runtime.
Neither has a second physical representation in the instructions to compare.
No proprietary decoder is used. Fields are from nvfx_shader.h and our emitters.
"""
import argparse
import json
import struct

from ucode_decode import unswap


# Actual source positions, not all encoded fields. Unused slots look like reads.
# VP ADD uses src0/src2; scalar slot uses src2 (nv40_vp_assembler.cpp).
VP_VECTOR_SOURCES = {
    0: (), 1: (0,), 2: (0, 1), 3: (0, 2), 4: (0, 1, 2),
    5: (0, 1), 6: (0, 1), 7: (0, 1), 8: (0, 1),
    9: (0, 1), 10: (0, 1), 11: (0, 1), 12: (0, 1),
    13: (0,), 14: (0,), 15: (0,), 16: (0, 1), 17: (),
    18: (0, 1), 19: (0, 1), 20: (0, 1), 21: (), 22: (0,),
    25: (0,),  # ARR/ARA not emitted here; leave their semantics unresolved.
}
VP_SCALAR_SOURCES = {0: (), **{op: (2,) for op in (1, 2, 3, 4, 5, 6, 7, 13, 14, 15, 16)}}
# Branch scalar opcodes deliberately unresolved: control-flow fields reuse bits.
FP_SOURCES = {
    0: (), 1: (1,), 2: (1, 2), 3: (1, 2), 4: (1, 2, 3),
    **{op: (1, 2) for op in range(5, 16)},
    **{op: (1,) for op in (0x10, 0x11, 0x13, 0x14, 0x15, 0x16,
                            0x17, 0x18, 0x1a, 0x1b, 0x1c, 0x1d,
                            0x22, 0x23, 0x24, 0x25, 0x27, 0x28,
                            0x29, 0x2a, 0x2f, 0x31, 0x39, 0x3c)},
    0x12: (), 0x19: (1, 2, 3), 0x20: (), 0x21: (),
    0x2e: (1, 2, 3), 0x38: (1, 2), 0x3a: (1, 2), 0x3b: (1, 2),
    0x3d: (), 0x3e: (),  # FENCTR/FENCBR: nv40_fp_assembler.cpp, no sources.
}


class Malformed(ValueError):
    pass


class Container:
    def __init__(self, blob):
        self.blob = blob
        self.region(0, 32)
        h = self.words(0, 8)
        self.profile, revision, size, count, table, _, self.ucode_size, self.ucode = h
        if self.profile not in (7003, 7004):
            raise Malformed('unsupported container profile %d' % self.profile)
        if size != len(blob):
            raise Malformed('declared size %d differs from file size %d' % (size, len(blob)))
        self.region(table, count * 48)
        self.region(self.ucode, self.ucode_size)
        if self.ucode % 16 or self.ucode_size % 16:
            raise Malformed('ucode offset/size is not 16-byte aligned')
        fixed = [(0, 32), (table, table + count * 48), (self.ucode, self.ucode + self.ucode_size)]
        for i, (start, end) in enumerate(fixed):
            if any(start < other_end and other_start < end for other_start, other_end in fixed[:i]):
                raise Malformed('header, parameter table and ucode overlap')
        metadata = []

        def metadata_region(offset, size):
            self.region(offset, size)
            span = (offset, offset + size)
            for start, end in fixed + metadata:
                if span != (start, end) and offset < end and start < offset + size:
                    raise Malformed('metadata blocks overlap at %d' % offset)
                if (start, end) in fixed and offset < end and start < offset + size:
                    raise Malformed('metadata overlaps a fixed container region at %d' % offset)
            metadata.append(span)
        self.records = []
        for i in range(count):
            fields = self.words(table + 48 * i, 12)
            name = self.string(fields[4])
            record = dict(name=name, index=i, type=fields[0], resource=fields[1],
                          variability=fields[2], register=fields[3], default=fields[5],
                          embedded=fields[6], paramno=fields[9], referenced=fields[10])
            if record['default']:
                if record['default'] % 16:
                    raise Malformed('default block is not 16-byte aligned')
                metadata_region(record['default'], 16)
            if record['embedded']:
                at = record['embedded']
                n = self.words(at, 1)[0]
                if at % 4:
                    raise Malformed('embedded-offset list is not word aligned')
                metadata_region(at, 4 + 4 * n)
                record['offsets'] = self.words(at + 4, n)
            else:
                record['offsets'] = ()
            self.records.append(record)

    def region(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.blob) - size:
            raise Malformed('out-of-bounds region at %d size %d' % (offset, size))

    def words(self, offset, count):
        self.region(offset, count * 4)
        return struct.unpack_from('>%dI' % count, self.blob, offset)

    def string(self, offset):
        if not offset:
            return ''
        self.region(offset, 1)
        end = self.blob.find(b'\0', offset)
        if end < 0:
            raise Malformed('unterminated string at %d' % offset)
        return self.blob[offset:end].decode('utf-8', errors='strict')


def _leaf(record):
    # Matrix parent types are 1x1..4x4. Struct/array parents have no usable
    # register resource. A matrix parent shares row zero; never count it twice.
    return record['variability'] in (4102, 4103) and not 1049 <= record['type'] <= 1064


def check_container(blob, expected_profile=None):
    """Return profile, checks, issues and decoded-read evidence; never skip silently.

    Each check has status verified / mismatch / unresolved / not_applicable.
    Issues have stable code and record/paramno identity for corpus allowances.
    Malformed bytes are reported rather than leaking parser exceptions.
    """
    result = dict(profile=None, checks=[], issues=[], reads=[], inline_blocks=[])

    def check(record, code, status, detail, **evidence):
        row = dict(record=record['name'] if record else '<ucode>',
                   paramno=record['paramno'] if record else None,
                   code=code, status=status, detail=detail, **evidence)
        result['checks'].append(row)
        if status in ('mismatch', 'unresolved'):
            result['issues'].append(row)

    try:
        container = Container(blob)
        result['profile'] = container.profile
        if expected_profile is not None and container.profile != expected_profile:
            check(None, 'container_profile_mismatch', 'mismatch',
                  'requested profile %d, container declares %d' % (expected_profile, container.profile))
            return result
        if not container.ucode_size:
            check(None, 'empty_ucode', 'unresolved', 'no instruction evidence is available')
        if container.profile == 7004:
            _check_fp(container, result, check)
        else:
            _check_vp(container, result, check)
    except (Malformed, UnicodeError, struct.error) as error:
        check(None, 'malformed_container', 'mismatch', str(error))
    return result


def _check_fp(c, result, check):
    blocks = set()
    cursor = 0
    while cursor < c.ucode_size:
        w = tuple(unswap(x) for x in c.words(c.ucode + cursor, 4))
        op = (w[0] >> 24) & 63
        if w[2] & 0x80000000 or op not in FP_SOURCES:
            check(None, 'unknown_fp_encoding', 'unresolved',
                  'cannot establish instruction boundaries after opcode %d' % op, offset=cursor)
            return  # Later words may be data, so guessing a walk would be false evidence.
        has_constant = any((w[i] & 3) == 2 for i in FP_SOURCES[op])
        if has_constant:
            if cursor + 32 > c.ucode_size:
                raise Malformed('FP inline block extends beyond ucode')
            blocks.add(cursor + 16)
        cursor += 32 if has_constant else 16
    result['inline_blocks'] = sorted(blocks)
    for record in c.records:
        if record['resource'] == 2178:
            check(record, 'fp_register_metadata', 'not_applicable',
                  'explicit C binding is metadata; FP reads embedded constants')
        if 1049 <= record['type'] <= 1064:
            check(record, 'aggregate_parent', 'not_applicable', 'matrix values belong to row records')
            # Aggregate liveness does not excuse a malformed relocation. Walk
            # any listed offsets even though normal parents have no such list.
        seen = set()
        for offset in record['offsets']:
            if offset in seen:
                check(record, 'duplicate_embedded_offset', 'mismatch', 'same relocation listed twice', offset=offset)
            seen.add(offset)
            if offset not in blocks:
                check(record, 'invalid_embedded_offset', 'mismatch', 'relocation does not point to a decoded inline block', offset=offset)
                continue
            check(record, 'embedded_block', 'verified', 'relocation points to a consumed inline block', offset=offset)
            if record['default']:
                expected = c.words(record['default'], 4)
                actual = tuple(unswap(x) for x in c.words(c.ucode + offset, 4))
                check(record, 'default_inline_mismatch' if actual != expected else 'default_inline',
                      'mismatch' if actual != expected else 'verified',
                      'compare all four raw float words, including padding', offset=offset,
                      expected=list(expected), actual=list(actual))
        if record['default'] and not record['offsets']:
            if record['referenced']:
                check(record, 'referenced_default_without_inline_use', 'unresolved',
                      'referenced FP default has no listed inline occurrence to verify')
            else:
                check(record, 'default_without_inline_use', 'not_applicable',
                      'unreferenced default has no instruction-side occurrence')


def _check_vp(c, result, check):
    reads = set()
    indirect = []
    complete = True
    for offset in range(0, c.ucode_size, 16):
        w = c.words(c.ucode + offset, 4)
        vec, scalar = (w[1] >> 22) & 31, (w[1] >> 27) & 31
        if vec not in VP_VECTOR_SOURCES or scalar not in VP_SCALAR_SOURCES:
            check(None, 'unknown_vp_encoding', 'unresolved',
                  'unknown vector/scalar opcode %d/%d' % (vec, scalar), offset=offset)
            complete = False
            continue
        sources = (((w[1] & 255) << 9) | (w[2] >> 23),
                   (w[2] >> 6) & 0x1ffff,
                   ((w[2] & 63) << 11) | (w[3] >> 21))
        for slot in sorted(set(VP_VECTOR_SOURCES[vec] + VP_SCALAR_SOURCES[scalar])):
            if sources[slot] & 3 != 3:
                continue
            register = (w[1] >> 12) & 511
            item = dict(offset=offset, slot=slot, register=register, indirect=bool(w[3] & 2))
            result['reads'].append(item)
            if item['indirect']:
                indirect.append(item)
            else:
                reads.add(register)
    declarations = {}
    matrix_rows = set()
    for parent in c.records:
        if 1049 <= parent['type'] <= 1064:
            rows = (parent['type'] - 1049) // 4 + 1
            matrix_rows.update((parent['name'] + '[%d]' % i, parent['paramno']) for i in range(rows))
    for record in c.records:
        # CG_CONSTANT records account for literal-pool C reads as well.
        if record['variability'] not in (4102, 4103):
            continue
        if record['default']:
            check(record, 'vp_runtime_default', 'not_applicable', 'default uploaded by runtime; no inline duplicate')
        if not _leaf(record):
            check(record, 'aggregate_parent', 'not_applicable', 'matrix parent shares row zero')
            continue
        if record['resource'] != 2178 or record['register'] == 0xffffffff:
            check(record, 'no_constant_register', 'not_applicable', 'no physical C register declared')
            continue
        register = record['register']
        if record['referenced']:
            declarations.setdefault(register, []).append(record)
        else:
            check(record, 'unreferenced_uniform', 'not_applicable', 'no read required for an unreferenced declaration')
            continue
        if register in reads:
            check(record, 'uniform_register_read', 'verified', 'declared register is read by an active operand', register=register)
        elif (record['name'], record['paramno']) in matrix_rows:
            # Measured on vp_matrix_row_small_v: reference and ours mark
            # m3[0]/m3[1] referenced at c256/c257, but only m3[2] is read.
            # Row reference flags express aggregate matrix liveness. Actual
            # reads must still resolve to declared leaves in the reverse check.
            check(record, 'aggregate_row_liveness', 'not_applicable',
                  'matrix reference state does not require each row to be read', register=register)
        elif indirect or not complete:
            check(record, 'unresolved_uniform_read', 'unresolved',
                  'indirect or unknown instruction could read this register', register=register)
        else:
            check(record, 'unread_uniform_register', 'mismatch', 'referenced register has no decoded read', register=register)
    for register in sorted(reads - declarations.keys()):
        check(None, 'undeclared_constant_read', 'mismatch', 'constant read has no referenced leaf declaration', register=register)
    for item in indirect:
        check(None, 'indirect_constant_read', 'unresolved',
              'input-dependent address not established by container metadata', **item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('container')
    args = parser.parse_args()
    try:
        with open(args.container, 'rb') as source:
            report = check_container(source.read())
    except OSError as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))
    if any(x['code'] == 'malformed_container' for x in report['issues']):
        return 2
    return int(bool(report['issues']))


if __name__ == '__main__':
    raise SystemExit(main())

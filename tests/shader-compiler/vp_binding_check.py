"""Reusable VP binding checks for t_49f3cc72 / t_25fa9e31.

The evaluator covers the vector instruction subset used by binding probes.
Constants supplied by the caller use independently expected registers, not
registers copied from the container. Unsupported instructions fail explicitly.
This checks binding/read agreement; it is not a general VP emulator.
"""
import math
import re
import struct
from vp_words import decode


def container(blob):
    header = struct.unpack_from('>8I', blob)
    records = {}
    for i in range(header[3]):
        row = struct.unpack_from('>12I', blob, header[4]+48*i)
        name = blob[row[4]:blob.index(0, row[4])].decode()
        key = (name, row[9])
        if key in records:
            raise ValueError('duplicate record '+str(key))
        records[key] = row
    words, error = decode(blob)
    if error:
        raise ValueError(error)
    # vp_words decodes the vector half only. Do not silently ignore scalar
    # coissue if a future fixture starts using it (SCA_OPCODE is bits27..31).
    for offset in range(header[7], header[7]+header[6], 16):
        if struct.unpack_from('>I', blob, offset+4)[0] >> 27:
            raise ValueError('scalar coissue outside the binding evaluator subset')
    return records, words


def evaluate_bindings(blob, constants, inputs):
    records, words = container(blob)
    constants = dict(constants)
    declared = {r[3] for r in records.values() if r[2] == 4102 and r[1] == 2178 and r[10]}
    for (name, _), row in records.items():
        if name.startswith('internal-constant-'):
            if row[3] in constants or not row[5]:
                raise ValueError('literal slot overlaps a supplied uniform or lacks values')
            constants[row[3]] = list(struct.unpack_from('>4f', blob, row[5]))
            declared.add(row[3])
    registers = dict(inputs, A0=[0]*4, A1=[0]*4)
    reads = []

    def source(text):
        negate = text.startswith('-')
        base, swizzle = text.lstrip('-').rsplit('.', 1)
        if base.startswith('C'):
            if base.startswith('C['):
                match = re.fullmatch(r'C\[(A[01])\.([xyzw])\+(\d+)\]', base)
                if not match:
                    raise ValueError('unrecognized relative source '+base)
                addr, lane, offset = match.groups()
                slot = int(offset)+int(registers[addr]['xyzw'.index(lane)])
            else:
                slot = int(base[1:])
            if slot not in declared:
                raise ValueError('instruction reads unreflected constant C'+str(slot))
            reads.append(slot)
            value = constants[slot]
        else:
            value = registers[base]
        return [(-1 if negate else 1)*value['xyzw'.index(lane)] for lane in swizzle]

    for line in words:
        match = re.fullmatch(r'\d+ (\w+) dst=(\w+) mask=([xyzw-]+) src0=(\S+) src1=(\S+) src2=(\S+)', line)
        if not match:
            raise ValueError('unrecognized instruction '+line)
        op, dst, mask, *sources = match.groups()
        if op == 'NOP':
            continue
        a = source(sources[0])
        if op in ('MOV', 'FLR', 'ARL'):
            value = a if op == 'MOV' else [math.floor(v) for v in a]
        else:
            b = source(sources[2] if op == 'ADD' else sources[1])
            if op == 'MUL': value = [x*y for x,y in zip(a,b)]
            elif op == 'ADD': value = [x+y for x,y in zip(a,b)]
            elif op == 'MAD': value = [x*y+z for x,y,z in zip(a,b,source(sources[2]))]
            elif op in ('DP3','DP4'): value = [sum(x*y for x,y in zip(a[:int(op[-1])],b))]*4
            elif op == 'DPH': value = [sum(x*y for x,y in zip(a[:3],b))+b[3]]*4
            else: raise ValueError('unsupported binding instruction '+line)
        registers.setdefault(dst, [0]*4)
        for lane in mask:
            if lane != '-': registers[dst]['xyzw'.index(lane)] = value['xyzw'.index(lane)]
    return registers['o0'], reads

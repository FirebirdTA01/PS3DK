"""Bounded decoded execution and cost check for texture/alpha-kill/fog.

This is not an emulator: accept only the instructions in this fixture and
reject every unsupported form. The texture is a supplied constant RGBA value.
"""
import math
import struct
import sys
from pathlib import Path

from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words, unswap


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def f32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]


def execute(blob, texel, fog, distinct=False):
    regs = {}
    cc = None
    ended = False
    for w, const in instructions(ucode_words(blob)):
        op = (w[0] >> 24) & 63
        ended = bool(w[0] & 1)
        if op in (0, 0x3e):
            if ended:
                break
            continue
        cond = (w[1] >> 18) & 7
        if op == 0x12:
            require(cond == 5 and cc is not None, 'expected guarded NE kill')
            if any(cc[(w[1] >> (21 + 2 * lane)) & 3] != 0 for lane in range(4)):
                return None
            continue
        require(cond == 7, 'unexpected predicated arithmetic')
        require(op in (1, 2, 3, 4, 0x0a, 0x17), 'unsupported opcode %x' % op)
        require(not (w[0] & (1 << 7)), 'unexpected half register')
        require(not (w[0] & (1 << 31)), 'unexpected saturation')
        require(not (w[2] & (15 << 28)), 'unexpected scale/branch')
        args = []
        for slot in range(1, ARITY[op] + 1):
            s = source(w, slot)
            require(not s['half'], 'unexpected half source')
            if s['type'] == INPUT:
                inputs = {'TEX1': [0.25, 0.75, 0., 1.], 'TEX2': fog}
                if distinct:
                    inputs['TEX3'] = [0.875, 0.125, 0.75, 1. - fog[3]]
                require(s['name'] in inputs, 'unexpected varying')
                data = inputs[s['name']]
            elif s['type'] == CONST:
                require(const is not None, 'missing literal')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in const]
            else:
                require(s['type'] == TEMP and s['reg'] in regs, 'uninitialized register')
                data = regs[s['reg']]
            lanes = [data[(s['swizzle'] >> (2 * lane)) & 3] for lane in range(4)]
            if s['abs']:
                lanes = [abs(x) for x in lanes]
            if s['negate']:
                lanes = [-x for x in lanes]
            args.append(lanes)
        result = []
        for lane in range(4):
            x = args[0][lane]
            if op == 0x17:
                require(((w[0] >> 17) & 15) == 0, 'unexpected sampler')
                x = texel[lane]
            elif op == 0x0a:
                x = float(x < args[1][lane])
            else:
                if op in (2, 4):
                    x = f32(x * args[1][lane])
                if op == 3:
                    x = f32(x + args[1][lane])
                if op == 4:
                    x = f32(x + args[2][lane])
            result.append(x)
        mask = (w[0] >> 9) & 15
        if w[0] & (1 << 8):
            if cc is None:
                cc = [math.nan] * 4
            for lane in range(4):
                if mask & (1 << lane):
                    cc[lane] = result[lane]
        if not (w[0] & (1 << 30)):
            dst = regs.setdefault((w[0] >> 1) & 63, [math.nan] * 4)
            for lane in range(4):
                if mask & (1 << lane):
                    dst[lane] = result[lane]
        if ended:
            break
    require(ended and 0 in regs, 'missing colour/END')
    return regs[0]


def check_saturation_guard(blob):
    # Saturating sample.rgb - fog.rgb changes the negative green component.
    # Reject that unsupported encoded form instead of silently simulating ADD.
    mutated = bytearray(blob)
    offset = struct.unpack_from('>8I', blob)[7]
    for words, const in instructions(ucode_words(blob)):
        if (words[0] >> 24) & 63 == 3:
            struct.pack_into('>I', mutated, offset, unswap(words[0] | (1 << 31)))
            break
        offset += 32 if const is not None else 16
    else:
        raise AssertionError('fixture lacks the subtraction ADD')
    try:
        execute(mutated, [0.75, 0.25, 0.5, 0.5], [0.125, 0.875, 0.25, 0.25])
    except AssertionError as error:
        require(str(error) == 'unexpected saturation', 'wrong saturation refusal: %s' % error)
    else:
        raise AssertionError('checker accepted saturated subtraction')
    print('saturation mutation: 1 pass, 0 fail')


def check(path, order_only=False, distinct=False):
    blob = Path(path).read_bytes()
    for alpha in (0., 0.0078125, 0.125, 0.5, 1.):
        for weight in (0., 0.25, 1.):
            texel = [0.75, 0.25, 0.5, alpha]
            fog = [0.125, 0.875, 0.25, weight]
            blend = 1. - weight if distinct else weight
            want = None if alpha < 0.0157 else [
                fog[i] + (texel[i] - fog[i]) * blend for i in range(3)] + [alpha]
            got = execute(blob, texel, fog, distinct)
            require(got == want, 'colour/kill mismatch: %s != %s' % (got, want))
    print('%s values: 15 pass, 0 fail' % Path(path).stem)
    decoded = list(instructions(ucode_words(blob)))
    kills = [i for i, (w, _) in enumerate(decoded) if ((w[0] >> 24) & 63) == 0x12]
    require(len(kills) == 1, 'expected one kill')
    fog_reads = [i for i, (w, _) in enumerate(decoded)
                 if any(source(w, s)['type'] == INPUT and source(w, s)['name'] in ('TEX2', 'TEX3')
                        for s in range(1, ARITY[(w[0] >> 24) & 63] + 1))]
    header = struct.unpack_from('>8I', blob)
    slots = header[6] // 16
    registers = blob[header[5] + 18]
    print('fog cost: slots=%d registers=%d fog_reads=%s kill=%s' %
          (slots, registers, fog_reads, kills))
    require(fog_reads and min(fog_reads) > kills[0], 'fog materialized before alpha kill')
    if not order_only and not distinct:
        # This change covers scheduling and input preload sharing. Reaching
        # the reference's two registers requires the separately reviewed
        # texture/output composition change.
        require(slots <= 9 and registers <= 3, 'fog exceeds 9-slot/3-register bound')
    if distinct:
        # Distinct attribute selectors must survive legalization. Numeric
        # inputs above give their w lanes opposite values, so replacing one
        # by the other fails on the surviving pixels, not just on a field.
        inputs = {source(w, s)['name'] for w, _ in decoded
                  for s in range(1, ARITY[(w[0] >> 24) & 63] + 1)
                  if source(w, s)['type'] == INPUT}
        require({'TEX2', 'TEX3'} <= inputs, 'distinct varying was lost')
    else:
        check_saturation_guard(blob)


def check_unconditional_order(path):
    decoded = list(instructions(ucode_words(Path(path).read_bytes())))
    require([((w[0] >> 24) & 63) for w, _ in decoded] == [1, 1, 0x12],
            'literal-only kill displaced the input-to-output MOV')
    first = decoded[0][0]
    require(not (first[0] & (1 << 30)) and ((first[0] >> 1) & 63) == 0 and
            source(first, 1)['type'] == INPUT,
            'expected input-to-colour MOV before unconditional kill')
    require(decoded[-1][0][0] & 1, 'unconditional kill must retain END')
    print('unconditional kill ordering: 1 pass, 0 fail')


if __name__ == '__main__':
    if '--unconditional' in sys.argv[2:]:
        check_unconditional_order(sys.argv[1])
    else:
        check(sys.argv[1], '--order-only' in sys.argv[2:], '--distinct' in sys.argv[2:])

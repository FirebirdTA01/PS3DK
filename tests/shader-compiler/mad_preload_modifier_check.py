"""Numerical controls for the actual cached MAD preload branch.

Optional reference containers are private, freshly compiled from the authored
fixtures. CI needs no proprietary tool or copied reference output.
"""
import argparse
import struct
from pathlib import Path
from fog_order_check import execute, require
from fp_sources import instructions, ucode_words, source, unswap, ABS_BIT


def check(kind, words_path, ours_path, reference=None):
    words = [int(w) for w in Path(words_path).read_text().split()]
    code = struct.pack('>%dI' % len(words), *words)
    blob = struct.pack('>8I', 0x1b5c, 6, 64 + len(code), 0, 32, 32,
                       len(code), 64) + bytes(32) + code
    decoded = list(instructions(ucode_words(blob)))
    mads = [(w, c) for w, c in decoded if (w[0] >> 24) & 63 == 4]
    require(len(mads) == 1, 'expected one emitted MAD')
    second = source(mads[0][0], 2)
    require(second['negate'] == (kind == 'neg') and
            second['abs'] == (kind == 'abs'), 'emitter lost second-read modifier')
    mutated = bytearray(blob)
    offset = 64
    for w, const in decoded:
        if (w[0] >> 24) & 63 == 4:
            index, bit = (2, 17) if kind == 'neg' else ABS_BIT[2]
            struct.pack_into('>I', mutated, offset + index * 4,
                             unswap(w[index] & ~(1 << bit)))
            break
        offset += 32 if const is not None else 16
    programs = [blob, Path(ours_path).read_bytes()]
    if reference:
        programs.append(Path(reference).read_bytes())
    cases = 0
    mutation_detected = False
    for rgb in ([0.25, -0.5, 0.75], [-0.75, 0.5, -0.25]):
        for weight in (-1., -0.5, 0., 0.5, 1.):
            fog = list(rgb) + [weight]
            factor = -weight if kind == 'neg' else abs(weight)
            expected = [rgb[i] * factor + [0.125, 0.25, 0.5][i]
                        for i in range(3)] + [1.]
            for program in programs:
                require(execute(program, None, fog) == expected,
                        '%s arithmetic mismatch for %s' % (kind, fog))
                cases += 1
            mutation_detected |= execute(mutated, None, fog) != expected
    require(mutation_detected, 'modifier-loss mutation escaped numerical controls')
    print('%s: %d numerical pass, 1 modifier-loss control pass, 0 fail' % (kind, cases))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('directory', type=Path)
    p.add_argument('--reference-directory', type=Path)
    args = p.parse_args()
    for kind in ('neg', 'abs'):
        stem = 'fp_mad_shared_%s_f' % kind
        check(kind, args.directory / (kind + '.words'),
              args.directory / (stem + '-ours.fpo'),
              args.reference_directory / (stem + '-ref.fpo')
              if args.reference_directory else None)

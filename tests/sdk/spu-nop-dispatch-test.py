#!/usr/bin/env python3
"""Keep SPU nop instructions distinct from the generic .nop directive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def sections(path):
    """Read section contents independently of assembler/disassembler printing."""
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x01\x02':
        raise ValueError('expected big-endian ELF32')
    header = struct.unpack_from('>HHIIIIIHHHHHH', data, 16)
    if header[0:2] != (1, 23) or header[10] != 40:
        raise ValueError('expected SPU relocatable ELF with 40-byte sections')
    table = [struct.unpack_from('>10I', data, header[5] + i * 40)
             for i in range(header[11])]

    def contents(section):
        offset, length = section[4:6]
        if offset + length > len(data):
            raise ValueError('section outside object')
        return data[offset:offset + length]

    names = contents(table[header[12]])
    result = {}
    for section in table:
        start = section[0]
        end = names.index(b'\0', start)
        name = names[start:end].decode('ascii')
        if name.startswith('.text.'):
            if name in result:
                raise ValueError('duplicate test section')
            result[name] = contents(section)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--as', dest='assembler')
    parser.add_argument('--output', type=Path,
                        help='retain evidence in a fresh subdirectory here')
    args = parser.parse_args()
    dev = os.environ.get('PS3DEV')
    if not args.assembler and not dev:
        print('spu-nop-dispatch: SKIP (set PS3DEV or --as)')
        return 0
    assembler = args.assembler or str(Path(dev) / 'spu/bin/spu-elf-as')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix='spu-nop-', dir=args.output))
        cleanup = None
    else:
        cleanup = tempfile.TemporaryDirectory(prefix='spu-nop-')
        work = Path(cleanup.name)
    rows = []

    def assemble(name, source):
        src, obj = work / (name + '.s'), work / (name + '.o')
        src.write_text(source)
        command = [assembler, str(src), '-o', str(obj)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (work / (name + '.log')).write_text(run.stdout + run.stderr)
        ok = run.returncode == 0 and obj.is_file() and obj.stat().st_size > 0
        rows.append(dict(name=name + '-assembles', command=command,
                         source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),
                         rc=run.returncode, ok=ok))
        return sections(obj) if ok else {}

    def check(name, data, expected):
        rows.append(dict(name=name, ok=data == expected,
                         actual_bytes=None if data is None else len(data),
                         expected_bytes=len(expected)))

    try:
        # Public SPU opcode encoding: NOP has RT in bits 0..6 and base
        # 0x40200000. No oracle output or disassembler formatting is used.
        nop = bytes.fromhex('40200000')
        cases = [('bare', 'nop', nop), ('zero', 'nop 0', nop),
                 ('one', 'nop 1', bytes.fromhex('40200001')),
                 ('max', 'nop 127', bytes.fromhex('4020007f')),
                 ('directive', '.nop 12', nop * 3),
                 ('directive-rounded', '.nop 127', nop * 32)]
        source = ''.join('.section .text.' + name + ',"ax",@progbits\n ' +
                         spelling + '\n' for name, spelling, _ in cases)
        result = assemble('spellings', source)
        for name, _, expected in cases:
            check(name, result.get('.text.' + name), expected)

        # A register-prefixed operand must also remain an opcode. Keep this
        # separate: an old assembler may refuse it instead of emitting bytes.
        result = assemble('register', '.section .text.reg,"ax",@progbits\n nop $127\n')
        check('register', result.get('.text.reg'), bytes.fromhex('4020007f'))

        # Nine four-byte NOPs leave this branch well within REL9 range.
        # Treating their operands as byte counts instead puts it beyond 1020.
        # HBRR/BR bytes below pin the measured candidate output; they are not
        # an independent instruction-encoding oracle.
        source = ('.section .text.hint,"ax",@progbits\n'
                  ' hbrr branch,target\n' + ' nop 127\n' * 9 +
                  'branch:\n br target\ntarget:\n lnop\n')
        result = assemble('hint', source)
        body = result.get('.text.hint', b'')
        rows.append(dict(name='hint-retained-and-in-range', ok=(
            len(body) == 48 and body[4:40] == bytes.fromhex('4020007f') * 9
            and body[:4] == bytes.fromhex('1200058a')
            and body[40:44] == bytes.fromhex('32000080')
            and body[44:] == bytes.fromhex('00200000'))))
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired) as error:
        rows.append(dict(name='guard-error', ok=False, diagnostic=str(error)))
    finally:
        (work / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
        for row in rows:
            print('spu-nop-dispatch:', 'PASS' if row['ok'] else 'FAIL', row['name'])
        if args.output:
            print('spu-nop-dispatch: evidence', work)
        if cleanup:
            cleanup.cleanup()
    return 0 if rows and all(row['ok'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())

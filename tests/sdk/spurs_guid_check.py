"""Independent ELF oracle for the PS3DK SPURS job GUID content contract."""
import hashlib
from pathlib import Path
import struct

DOMAIN = b'PS3DK-SPU-GUID\x00\x01'


def inspect(path):
    data = Path(path).read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x02\x01':
        raise ValueError('expected ELF32 big-endian version 1')
    machine = struct.unpack_from('>H', data, 18)[0]
    entry, _, offset, flags = struct.unpack_from('>4I', data, 24)
    stride, count, names_index = struct.unpack_from('>3H', data, 46)
    if machine != 23 or flags not in (1, 2) or stride != 40 or not count:
        raise ValueError('expected a linked SPURS job ELF with section headers')

    def take(offset, size):
        if offset > len(data) or size > len(data)-offset:
            raise ValueError('section exceeds file bounds')
        return data[offset:offset+size]

    headers = [struct.unpack('>10I', take(offset+i*stride, stride)) for i in range(count)]
    if names_index >= count:
        raise ValueError('invalid section-name table')
    names = take(headers[names_index][4], headers[names_index][5])
    sections = []
    for h in headers:
        if h[0] >= len(names) or b'\0' not in names[h[0]:]:
            raise ValueError('invalid section name')
        name = names[h[0]:].split(b'\0', 1)[0]
        sections.append((name, h))
    guid = [(n,h) for n,h in sections if n == b'.SpuGUID']
    if len(guid) != 1:
        raise ValueError('expected exactly one .SpuGUID section')
    _, gh = guid[0]
    if gh[1] != 1 or gh[2] != 6 or gh[5] != 16:
        raise ValueError('GUID must be 16-byte AX PROGBITS')
    actual = take(gh[4], 16)
    words = struct.unpack('>4I', actual)
    if any((w & 0xfe0001ff) != (0x42000002 | (i << 7)) for i,w in enumerate(words)):
        raise ValueError('invalid indexed ILA encoding')

    records = sorted(((n,h) for n,h in sections
                      if h[2] & 2 and h[1] != 7 and n != b'.SpuGUID'),
                     key=lambda record: (record[1][3], record[0]))
    digest = hashlib.sha1(DOMAIN)
    digest.update(struct.pack('>3I', entry, flags, len(records)))
    for name,h in records:
        digest.update(struct.pack('>5I', h[3], h[1], h[2], h[5], len(name)))
        digest.update(name)
        if h[1] != 8:
            digest.update(take(h[4], h[5]))
    identity = digest.digest()[:8]
    chunks = struct.unpack('>4H', identity)
    expected = struct.pack('>4I', *(0x42000002 | (chunk << 9) | (i << 7)
                                  for i,chunk in enumerate(chunks)))
    if actual != expected:
        raise ValueError('GUID does not identify the final allocated content')
    return identity.hex()


if __name__ == '__main__':
    import sys
    try:
        for path in sys.argv[1:]:
            print(path, inspect(path))
    except (ValueError, OSError, struct.error) as error:
        print('GUID FAIL:', error, file=sys.stderr)
        raise SystemExit(1)

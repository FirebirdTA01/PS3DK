#!/usr/bin/env python3
"""Independent byte fixture/checker for the C serializer, also usable on target hex output."""
import struct
import sys
import zlib
from pathlib import Path


def expected():
    def text(value):
        return struct.pack('<H', len(value)) + value
    header = b''.join(text(s) for s in [b'app', b'build', b'sdk', b'gcc', b'-pg', b'work'])
    header += struct.pack('<BBHQQQQIII', 1, 1, 1, 17, 1, 0, 2**64-1, 0, 2, 16)
    chunks = [
        (1, header),
        (2, struct.pack('<IIIQQ', 1, 1, 0, 1000, 10)),
        (4, struct.pack('<IIIQ', 1, 1, 0, 42) + text(b'worker')),
        (8, struct.pack('<IIQQQII', 1, 1, 0x1004, 0x2004, 7, 0, 0)),
        (6, struct.pack('<IQiIIBBHQQQQ', 0, 0, 0, 1, 1, 2, 0, 0, 7, 7, 0, 0)),
    ]
    result = b'PS3DKPRF' + struct.pack('<HHI', 1, 0, 1)
    for seq, (kind, payload) in enumerate(chunks):
        frame = struct.pack('<IIQ', kind, len(payload), seq) + payload
        result += frame + struct.pack('<I', zlib.crc32(frame))
    return result


if __name__ == '__main__':
    actual = Path(sys.argv[1]).read_bytes()
    assert actual == expected(), 'C serializer differs from independent byte fixture/checker'
    print(f'oracle tests=1 pass=1 fail=0 bytes={len(actual)}')

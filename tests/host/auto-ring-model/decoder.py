"""Independent decoder for RGM1, a model-private format (never an SDK chunk).

The caller supplies the admission ledger; the payload cannot authenticate its
own owner/frame/generation. Synthetic timestamp values also detect stale copies.
"""
import struct


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def decode(blob, expected, capacity):
    require(len(blob) >= 48, "short block")
    magic, version, flags, seq, count, width, size, reserved = struct.unpack_from("<4sHHQIIII", blob)
    require(magic == b"RGM1" and version == 1, "format")
    require(flags == 1 and reserved == 0, "uncommitted or unknown header")
    require(seq == expected.seq and seq > 0, "generation")
    require(0 < count <= capacity and count == len(expected.ids), "count")
    require(width == 56 and size == count * 56 and len(blob) == 48 + size, "bounds")
    end_seq, complement = struct.unpack_from("<QQ", blob, 32 + size)
    require(end_seq == seq and complement == (seq ^ ((1 << 64) - 1)), "torn commit")
    rows, seen = [], set()
    for i in range(count):
        op, owner, frame, window, generation, begin, end = struct.unpack_from("<7Q", blob, 32 + i * 56)
        require(op == expected.ids[i] and op not in seen, "operation identity")
        require(owner == 7 and frame == expected.frame and window == expected.window, "scope identity")
        require(generation == seq, "copied generation")
        require(0 < begin <= end and (begin, end) == (op * 16 + 1, op * 16 + 6), "copied timestamps")
        seen.add(op)
        rows.append((op, frame, begin, end))
    return rows


def decode_stream(blobs, ledger, capacity):
    require(len(blobs) == len(ledger), "missing or extra generation")
    rows, ids = [], set()
    for seq, blob in enumerate(blobs, 1):
        require(seq in ledger, "missing ledger generation")
        decoded = decode(blob, ledger[seq], capacity)
        for row in decoded:
            require(row[0] not in ids, "duplicate stream operation")
            ids.add(row[0])
        rows.extend(decoded)
    return rows

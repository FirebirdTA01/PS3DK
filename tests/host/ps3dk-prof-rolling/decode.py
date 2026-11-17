#!/usr/bin/env python3
"""Independent stdlib decoder/control for the physical PS3DKROL foundation.

Consumes bytes, uses struct/zlib, and does not import/call the C codec. It does
not interpret opaque checkpoints or claim record-family restart validity.
"""
import argparse
import pathlib
import struct
import zlib

PAGE = 4096
PREFIX = 12288
U64_MAX = (1 << 64) - 1

class Invalid(ValueError):
    pass

def need(ok, reason):
    if not ok:
        raise Invalid(reason)

def num(data, at, code):
    try:
        return struct.unpack_from("<" + code, data, at)[0]
    except struct.error as exc:
        raise Invalid("short integer") from exc

def crc(data):
    return zlib.crc32(data) & 0xffffffff

def check_crc(data, at):
    need(num(data, at, "I") == crc(data[:at]), "CRC")

def chunk_sequences(payload, first):
    sequences = []
    offset = 0
    next_seq = first
    while offset < len(payload):
        need(len(payload) - offset >= 20, "torn chunk header")
        size = num(payload, offset + 4, "I")
        seq = num(payload, offset + 8, "Q")
        need(size <= 1048576 and seq == next_seq and seq < U64_MAX, "chunk size/sequence")
        end = offset + 20 + size
        need(end <= len(payload), "torn chunk payload")
        check_crc(payload[offset:end], end - offset - 4)
        sequences.append(seq)
        next_seq += 1
        offset = end
    return sequences, next_seq

def decode(data):
    need(len(data) >= PREFIX, "short container")
    header = data[:PAGE]
    need(header[:8] == b"PS3DKROL" and num(header, 8, "I") == 1, "magic/version")
    need(num(header, 12, "I") == PAGE and not any(header[64:-4]), "config reserved")
    check_crc(header, PAGE - 4)
    length, cap = num(header, 16, "Q"), num(header, 24, "Q")
    n, cb, pb, slot_size = struct.unpack_from("<4I", header, 32)
    identity = header[48:64]
    need(2 <= n <= 64 and pb >= 20, "config limits")
    wanted_slot = (96 + cb + pb + 64 + PAGE - 1) // PAGE * PAGE
    need(wanted_slot <= 0xffffffff and slot_size == wanted_slot, "slot arithmetic")
    need(length == PREFIX + (n + 1) * slot_size and len(data) == length <= cap, "cap/file length")

    def slot(entry):
        physical, cp_bytes, payload_bytes, segment, first, next_seq = entry
        at = PREFIX + physical * slot_size
        b = data[at:at + slot_size]
        footer = b[-64:]
        need(b[:8] == b"PFRSLOT1" and b[8:24] == identity, "slot identity")
        need(segment <= U64_MAX - 2, "slot generation overflow")
        need(struct.unpack_from("<4Q", b, 24) == (segment, segment + 2, first, next_seq), "slot cursors")
        need(struct.unpack_from("<2I", b, 56) == (cp_bytes, payload_bytes), "slot lengths")
        cp_end = 96 + cp_bytes
        data_end = cp_end + payload_bytes
        need(data_end <= slot_size - 64, "slot bounds")
        cp = b[96:cp_end]
        payload = b[cp_end:data_end]
        cc, pc = crc(cp), crc(payload)
        need(struct.unpack_from("<2I", b, 64) == (cc, pc), "checkpoint/payload CRC")
        need(not any(b[72:80]) and not any(b[84:96]) and not any(b[data_end:-64]), "slot reserved/padding")
        check_crc(b, 80)
        need(footer[:8] == b"PFRCMIT1", "slot commit footer")
        need(struct.unpack_from("<4Q", footer, 8) == (segment, segment + 2, first, next_seq), "footer cursors")
        need(struct.unpack_from("<3I", footer, 40) == (cc, pc, crc(b[:-64])), "footer checksums")
        need(not any(footer[52:60]), "footer reserved")
        check_crc(footer, 60)
        seqs, end = chunk_sequences(payload, first)
        need(end == next_seq, "slot next sequence")
        return {"segment": segment, "physical": physical, "first": first, "next": next_seq,
                "checkpoint": cp, "payload": payload, "sequences": seqs}

    def root(which):
        b = data[PAGE * (which + 1):PAGE * (which + 2)]
        need(b[:8] == b"PFRROOT1" and b[80:96] == identity, "root identity")
        check_crc(b, PAGE - 4)
        gen, count, reserved = struct.unpack_from("<QII", b, 8)
        first_seg, next_seg, first, end, dropped_seg, dropped_chunks, dropped_bytes = struct.unpack_from("<7Q", b, 24)
        need(gen > 0 and gen % 2 == which and next_seg < U64_MAX and gen == next_seg + 1, "root generation")
        need(reserved == 0 and count <= n and first_seg <= next_seg and next_seg - first_seg == count, "root segment range")
        need(first_seg == dropped_seg and first == dropped_chunks and first <= end, "root prefix accounting")
        need(dropped_bytes <= dropped_seg * pb, "evicted byte ceiling")
        need(count == min(next_seg, n), "root retained count")
        if not count:
            need(gen == 1 and first == end == dropped_bytes == 0, "empty root")
        need(not any(b[96 + count * 40:-4]), "root unused bytes")
        entries = []
        seen = set()
        cursor = first
        for i in range(count):
            physical, cp_bytes, payload_bytes, zero, seq, start, stop = struct.unpack_from("<4I3Q", b, 96 + i * 40)
            need(physical <= n and physical not in seen and zero == 0, "root slot identity")
            need(cp_bytes <= cb and payload_bytes <= pb, "root slot limits")
            need(seq == first_seg + i and start == cursor and start <= stop and stop - start <= payload_bytes // 20,
                 "root chunk chain")
            seen.add(physical)
            entries.append(slot((physical, cp_bytes, payload_bytes, seq, start, stop)))
            cursor = stop
        need(cursor == end, "root final frontier")
        return {"generation": gen, "first_segment": first_seg, "next_segment": next_seg,
                "first_chunk": first, "next_chunk": end, "evicted_segments": dropped_seg,
                "evicted_chunks": dropped_chunks, "evicted_payload_bytes": dropped_bytes,
                "segments": entries}

    valid = []
    failures = []
    missing = []
    for which in range(2):
        missing.append(not any(data[(which + 1) * PAGE:(which + 2) * PAGE]))
        try:
            valid.append(root(which))
        except Invalid as exc:
            failures.append((which, str(exc)))
    need(valid, "no complete valid root chain")
    chosen = max(valid, key=lambda item: item["generation"])
    chosen["recovered"] = any(not missing[which] for which, _ in failures) or (any(missing) and chosen["generation"] != 1)
    chosen["file_bytes"] = length
    return chosen

def encode_control():
    """Hand-specified 3-commit/2-retained fixture, solely to exercise oracle negatives."""
    identity = bytes([0x42]) + bytes(15)
    data = bytearray(24576)
    struct.pack_into("<8sIIQQ4I16s", data, 0, b"PS3DKROL", 1, PAGE, 24576, 24576, 2, 16, 64, PAGE, identity)
    struct.pack_into("<I", data, PAGE - 4, crc(data[:PAGE - 4]))
    for seq in range(3):
        b = bytearray(PAGE)
        payload = struct.pack("<IIQI", 43, 4, seq, seq ^ 0xaabbccdd)
        payload += struct.pack("<I", crc(payload))
        cp = struct.pack("<Q", seq)
        struct.pack_into("<8s16s4Q4I", b, 0, b"PFRSLOT1", identity, seq, seq + 2, seq, seq + 1,
                         8, 24, crc(cp), crc(payload))
        struct.pack_into("<I", b, 80, crc(b[:80]))
        b[96:128] = cp + payload
        struct.pack_into("<8s4Q3I", b, PAGE - 64, b"PFRCMIT1", seq, seq + 2, seq, seq + 1,
                         crc(cp), crc(payload), crc(b[:-64]))
        struct.pack_into("<I", b, PAGE - 4, crc(b[-64:-4]))
        data[PREFIX + seq * PAGE:PREFIX + (seq + 1) * PAGE] = b
    for which, generation, first, end in [(1, 3, 0, 2), (0, 4, 1, 3)]:
        b = bytearray(PAGE)
        struct.pack_into("<8sQII7Q16s", b, 0, b"PFRROOT1", generation, 2, 0,
                         first, end, first, end, first, first, first * 24, identity)
        for i, seq in enumerate(range(first, end)):
            struct.pack_into("<4I3Q", b, 96 + i * 40, seq, 8, 24, 0, seq, seq, seq + 1)
        struct.pack_into("<I", b, PAGE - 4, crc(b[:-4]))
        data[(which + 1) * PAGE:(which + 2) * PAGE] = b
    return data

def self_test():
    data = encode_control()
    got = decode(data)
    need(got["generation"] == 4 and got["first_chunk"] == 1 and got["next_chunk"] == 3, "control window")
    # Corrupt newest unique slot: old root remains complete, no cross-root stitching.
    mutated = data.copy()
    mutated[PREFIX + 2 * PAGE] ^= 1
    got = decode(mutated)
    need(got["generation"] == 3 and got["recovered"], "control fallback")
    # Shared slot corruption invalidates both complete chains.
    cases = []
    mutated = data.copy(); mutated[PREFIX + PAGE] ^= 1; cases.append(mutated)
    mutated = data.copy(); mutated[0] ^= 1; cases.append(mutated)
    cases.append(data[:-1])
    # Valid root CRC must not hide an impossible cumulative eviction claim.
    mutated = data.copy()
    for which in (0, 1):
        offset = (which + 1) * PAGE
        struct.pack_into("<Q", mutated, offset + 64, 99)
        struct.pack_into("<I", mutated, offset + PAGE - 4, crc(mutated[offset:offset + PAGE - 4]))
    cases.append(mutated)
    for item in cases:
        try:
            decode(item)
        except Invalid:
            continue
        raise Invalid("negative control accepted")
    print("PASS independent decoder controls (Python fixture; not a C execution)")

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", type=pathlib.Path, nargs="?")
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--expect-rotations", action="store_true")
    args = p.parse_args()
    if args.self_test:
        self_test()
    if args.capture:
        result = decode(args.capture.read_bytes())
        if args.expect_rotations:
            need((result["file_bytes"], result["generation"], result["first_chunk"], result["next_chunk"],
                  result["evicted_segments"], result["evicted_chunks"], result["evicted_payload_bytes"])
                 == (28672, 13, 9, 12, 9, 9, 216), "C three-rotation output")
            need([s["segment"] for s in result["segments"]] == [9, 10, 11], "C retained segment identities")
            for seg, seq in zip(result["segments"], [9, 10, 11]):
                need(seg["checkpoint"] == struct.pack("<Q", seq) and seg["sequences"] == [seq], "C checkpoint/payload")
        print(f"PASS independent decode: generation {result['generation']}, chunks "
              f"[{result['first_chunk']},{result['next_chunk']}), {result['file_bytes']} bytes")
    elif not args.self_test:
        p.error("provide a capture or --self-test")

if __name__ == "__main__":
    main()

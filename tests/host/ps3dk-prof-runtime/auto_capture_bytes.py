"""Byte-level comparison of two AUTO host captures with an explicit mask of run-dependent fields.

Two runs of the same program differ only in values that depend on the run, never in what was recorded. The
comparison is byte for byte except exactly these fields (every one a whole field, never a partial byte range):

  HEADER (1)      session id: the init timebase value (u64 at payload length - 20)
  CLOCKS (2)      tb0: the timebase at init (u64 at 20)
  PROFILE (17)    runtime archive digest (32 bytes after version, reserved, id length, id and digest algorithm):
                  the host harness builds the archive in each run's own directory and -g records that path
  THREADS (4)     each thread's OS thread id (u64 at 8 of each entry; entries are u32 inc, u32 slot, u64 os, string)
  RECORDS (5)     the timebase of BEGIN (1), END (3) and FRAME (4) records (u64 at 16 of each 32-byte record)
  EPOCH (9)       the epoch's timebase (u64 at 24)
  AUTO_OPS (35)   each operation's tb_before and tb_after (u64 at 24 and 32 of each record)
  AUTO_COVERAGE (36) the row's tb_begin and tb_end (u64 at 24 and 32)
  AUTO_CLOCK (38) the anchor's tb_before and tb_after (u64 at 32 and 40)

Two further things depend on when the drainer runs, not on what was recorded: the ORDER of chunks of different
families (the drainer interleaves base, AUTO and FIFO chunks as they become ready) and how many records one RECORDS
(5) or AUTO_OPS (35) chunk carries (one drain may write a batch as one chunk or two). So the comparison is per chunk
type, and those two batched families are compared as one record stream: RECORDS per incarnation (each chunk's 8-byte
header names the incarnation and the count), AUTO_OPS with every chunk header equal except its count. Every other
chunk must match one for one. The 16-byte file header must match exactly. Nothing else is masked.
"""
import struct


def chunks(data):
    out, pos = [], 16
    while pos + 20 <= len(data):
        ty, ln = struct.unpack_from('<II', data, pos)
        out.append((ty, data[pos + 16:pos + 16 + ln]))
        pos += 20 + ln
    if pos != len(data):
        raise ValueError(f'trailing {len(data) - pos} bytes')
    return data[:16], out


def _strings_end(p, at, n):
    for _ in range(n):
        ln = struct.unpack_from('<H', p, at)[0]
        at += 2 + ln
    return at


def mask(ty, p):
    """The masked (offset, length) fields of one chunk payload."""
    m = []
    if ty == 1:
        m.append((len(p) - 20, 8))
    elif ty == 2:
        m.append((20, 8))
    elif ty == 17:
        n = p[4]
        m.append((5 + n + 4, 32))
    elif ty == 4:
        count = struct.unpack_from('<I', p, 0)[0]
        at = 4
        for _ in range(count):
            m.append((at + 8, 8))
            at = _strings_end(p, at + 16, 1)
    elif ty == 5:
        count = struct.unpack_from('<I', p, 4)[0]
        for k in range(count):
            r = 8 + 32 * k
            if p[r] in (1, 3, 4):
                m.append((r + 16, 8))
    elif ty == 9:
        m.append((24, 8))
    elif ty == 35:
        count, width = struct.unpack_from('<II', p, 8)
        for k in range(count):
            r = 16 + width * k
            m += [(r + 24, 8), (r + 32, 8)]
    elif ty == 36:
        m += [(24, 8), (32, 8)]
    elif ty == 38:
        m += [(32, 8), (40, 8)]
    return m


def _merge(ty, payloads):
    """RECORDS and AUTO_OPS as record streams: a synthetic payload per stream, the header's count rewritten."""
    if ty == 5:
        streams = {}
        for p in payloads:
            inc, count = struct.unpack_from('<II', p, 0)
            streams.setdefault(inc, []).append(p[8:8 + 32 * count])
        return [struct.pack('<II', inc, sum(len(r) for r in rs) // 32) + b''.join(rs) for inc, rs in sorted(streams.items())]
    if ty == 35:
        if not payloads:
            return []
        heads = {p[:8] + p[12:16] for p in payloads}
        if len(heads) != 1:
            return payloads  # different headers: compared chunk by chunk (and reported)
        width = struct.unpack_from('<I', payloads[0], 12)[0]
        body = b''.join(p[16:] for p in payloads)
        return [payloads[0][:8] + struct.pack('<I', len(body) // width) + payloads[0][12:16] + body]
    return payloads


def compare(a, b):
    """(problems, masked_bytes_that_differed): problems is empty when a and b match outside the mask."""
    ha, ca = chunks(a)
    hb, cb = chunks(b)
    problems, masked = [], 0
    if ha != hb:
        problems.append(f'file header {ha.hex()} != {hb.hex()}')
    types = sorted({t for t, _ in ca} | {t for t, _ in cb})
    for t in types:
        xa = _merge(t, [p for k, p in ca if k == t])
        xb = _merge(t, [p for k, p in cb if k == t])
        if len(xa) != len(xb):
            problems.append(f'type {t}: {len(xa)} chunks != {len(xb)}')
            continue
        for i, (x, y) in enumerate(zip(xa, xb)):
            if len(x) != len(y):
                problems.append(f'type {t} #{i}: {len(x)} bytes != {len(y)}')
                continue
            mx, my = mask(t, x), mask(t, y)
            if mx != my:
                problems.append(f'type {t} #{i}: masks differ')
                continue
            covered = bytearray(len(x))
            for off, ln in mx:
                covered[off:off + ln] = b'\x01' * ln
            bad = [k for k in range(len(x)) if x[k] != y[k] and not covered[k]]
            masked += sum(1 for k in range(len(x)) if x[k] != y[k] and covered[k])
            if bad:
                problems.append(f'type {t} #{i}: {len(bad)} unmasked bytes differ, first at {bad[0]}')
    return problems, masked

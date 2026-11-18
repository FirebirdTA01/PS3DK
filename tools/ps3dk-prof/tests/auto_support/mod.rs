//! Shared AUTO RSX fixtures (reader + viewer tests): a capture whose clock map is qualified and whose
//! frames carry per-frame RSX busy time.  `qualified_busy()` builds it; the reader test pins the reader's
//! per-frame numbers and writes the committed fixture, the viewer test draws the Frames page's RSX lane
//! from the same bytes and checks its drawn values against the reader.
//!
//! Layout (base PPU owner 1, three frames 0/1/2):
//!   frame 0: one covered CLEAR (raw 6000..36_000) -> RSX busy 30_000 raw, mapped to a PPU range
//!   frame 1: no covered operation -> RSX busy unavailable (the reader's reason carries)
//!   frame 2: a base PPU frame with no AUTO row -> not an AUTO frame at all (no lane mark)
#![allow(dead_code)]

use super::common::*;
use super::fifo_support::*;

const AUTO_FLAGS: u32 = 1 | 64 | 128;

fn cfg127() -> Chunk {
    let mut b = Buf::default();
    //       domain epoch owner gen kinds forms first count  op   clock pairs meta frames width
    let words: [u32; 14] = [D, 1, OWNER, 1, 127, 11, 1024, 1024, 239, 238, 504, 1024, 256, 144];
    b.u16(1).u16(0).u32(1);
    for w in words {
        b.u32(w);
    }
    b.u64(FREQ).u64(240_384).u64(2 * 1024 * 1024).u32(3).u32(1);
    b.0.extend_from_slice(&[0x2a; 32]);
    b.0.extend_from_slice(&[0x63; 32]);
    chunk(34, b.0)
}
fn anchors() -> Vec<Vec<u8>> {
    (0..9u64).map(|i| {
        let f = 1000 + 50_000 * i;
        let id = i as u32 + 1;
        let (before, after) = (2 * f - 1, 2 * f + 1);
        let mut b = Buf::default();
        // schema flags instance epoch id role slot serial polls tb_before tb_after raw repeated label_first label_second frame owner r0 elapsed
        b.u16(1).u16(1).u32(1).u32(1).u32(id).u32(0).u32(1024 + (id - 1) % 16).u32(id).u32(1).u64(before).u64(after)
            .u64(f).u64(f).u32(0).u32(2 * id).u64(0).u32(OWNER).u32(0).u64(after - before);
        b.0
    })
    .collect()
}
// id, frame, kind (3 cover, 4 mark), raw (b..e), args
fn aop(id: u64, frame: u64, kind: u16, raw: (u64, u64), args: [u32; 7]) -> Vec<u8> {
    let s = 2 * id as u32 - 1;
    let mut b = Buf::default();
    b.u64(id).u32(OWNER).u16(kind).u16(3).u64(frame).u64(10).u64(20).u64(raw.0).u64(raw.1).u64(0x1234);
    for a in args {
        b.u32(a);
    }
    b.u32(0)
        .u32(1040 + 2 * (id as u32 % 500))
        .u32(1041 + 2 * (id as u32 % 500))
        .u32(s)
        .u32(s + 1)
        .u32(1)
        .u32(0)
        .u64(u64::from(s + 1));
    b.u64(0).u32(0).u16(0).u16(0); // 144-byte build tail
    b.0
}
fn opchunk(list: &[Vec<u8>]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(list.len() as u32).u32(144);
    for o in list {
        b.0.extend_from_slice(o);
    }
    chunk(35, b.0)
}
fn rowchunk(frame: u64, tb: (u64, u64), clears: u64) -> Chunk {
    let mut rows = [[0u64; 12]; 3];
    rows[2][0] = clears;
    rows[2][1] = clears;
    let mut b = Buf::default();
    b.u16(1).u16(67).u32(1).u32(OWNER).u32(0).u64(frame).u64(tb.0).u64(tb.1).u64(0).u64(0).u64(0);
    for k in rows {
        for v in k {
            b.u64(v);
        }
    }
    b.u32(0).u32(0).u32(0).u32(0).u64(0).u32(0).u32(0);
    chunk(36, b.0)
}
fn clockchunk(a: &[u8]) -> Chunk {
    chunk(38, a.to_vec())
}
fn statuschunk(flags: u16, n: [u64; 15]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(flags).u32(1);
    for v in n {
        b.u64(v);
    }
    chunk(37, b.0)
}
fn ff_wrap(seq0: u64, id: u64, t: u64, p1: u64, p2: u64, frame: u64) -> Vec<Ev> {
    wrap(seq0, id, t, p1, p2).into_iter().map(|mut e| {
        e.frame = frame;
        e
    })
    .collect()
}

/// The combined capture: base PPU owner 1 (frames 0/1/2) + AUTO + FIFO + clock anchors.
pub fn qualified_busy() -> File {
    let recs = seqd(0, [frame(0, 0, 0), frame(0, 1000, 1), frame(0, 2000, 2)].to_vec());
    let mut f = scenario(vec![lane(1, recs)]).file();
    f.flags = AUTO_FLAGS;
    let trail = f.find(T_TRAILER);
    let mut insert = vec![rsx_domain(D), Cfg::default().chunk()];
    let mut evs = ff_wrap(1, 1, 20_000, 20_000, 10_000, 0);
    insert.push(events(&evs));
    insert.push(St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk());
    let list = [
        aop(1, 0, 4, (5000, 5001), [1, 0, 0, 0, 0, 0, 0]),
        aop(2, 0, 3, (6000, 36_000), [6, 0, 0, 0, 0, 0, 0xf3]),
        aop(3, 0, 4, (36_500, 36_501), [2, 0, 0, 0, 0, 0, 0]),
        aop(4, 1, 4, (100_000, 100_001), [1, 0, 0, 0, 0, 0, 0]),
        aop(5, 1, 4, (200_000, 200_001), [2, 0, 0, 0, 0, 0, 0]),
    ];
    insert.push(cfg127());
    insert.push(opchunk(&list));
    insert.push(rowchunk(0, (10_000, 110_000), 1));
    insert.push(rowchunk(1, (110_000, 210_000), 0));
    for a in anchors() {
        insert.push(clockchunk(&a));
    }
    insert.push(statuschunk(1, [5, 5, 5, 0, 0, 2, 0, 0, 0, 9, 9, 0, 0, 10, 0]));
    f.chunks.splice(trail..trail, insert);
    f
}

//! FIFO wrap-event fixtures (docs/design/ps3dk-prof-fifo.md), shared by the reader, CLI and viewer tests.
#![allow(dead_code)]
use super::common::*;

pub const D: u32 = 1026;
pub const OWNER: u32 = 1;
pub const FREQ: u64 = 79_800_000;
pub const BEGIN: u32 = 0x1000;
pub const END: u32 = 0x1000 + 4 * 1023;
pub const TAIL: u32 = 0x1000 + 4 * 1000;
pub const T_DOMAIN: u32 = 8;

pub fn rsx_domain(id: u32) -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(id).u8(2).u8(64).u8(2).u8(0).u32(2).u32(0).u64(0).u64(0).u64(0).u64(0).str("rsx");
    chunk(T_DOMAIN, b.0)
}

#[derive(Clone, Copy)]
pub struct Cfg {
    pub schema: u16,
    pub features: u16,
    pub instance: u32,
    pub domain: u32,
    pub owner: u32,
    pub record: u32,
    pub ring: u32,
    pub max: u32,
    pub allocated: u32,
    pub freq: u64,
    pub begin: u32,
    pub end: u32,
    pub generation: u32,
    pub r0: u32,
    pub r1: u64,
}
impl Default for Cfg {
    fn default() -> Self {
        Self { schema: 1, features: 1, instance: 1, domain: D, owner: OWNER, record: 96, ring: 1024, max: 1 << 20,
            allocated: 1024 * 96 + 4096, freq: FREQ, begin: BEGIN, end: END, generation: 1, r0: 0, r1: 0 }
    }
}
impl Cfg {
    pub fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        b.u16(self.schema).u16(self.features).u32(self.instance).u32(self.domain).u32(self.owner).u32(self.record)
            .u32(self.ring).u32(self.max).u32(self.allocated).u64(self.freq).u32(self.begin).u32(self.end)
            .u32(self.generation).u32(self.r0).u64(self.r1);
        chunk(31, b.0)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Ev {
    pub seq: u64,
    pub wrap: u64,
    pub t0: u64,
    pub t1: u64,
    pub owner: u32,
    pub kind: u16,
    pub flags: u16,
    pub frame_owner: u32,
    pub z0: u32,
    pub frame: u64,
    pub begin: u32,
    pub tail: u32,
    pub target: u32,
    pub words: u32,
    pub current: u32,
    pub z1: u32,
    pub z2: u64,
    pub z3: u64,
}
pub fn ev(seq: u64, wrap: u64, kind: u16, t0: u64, t1: u64) -> Ev {
    let target = match kind {
        2 | 3 => TAIL,
        4 | 5 => BEGIN,
        _ => 0,
    };
    Ev { seq, wrap, t0, t1, owner: OWNER, kind, flags: 1, frame_owner: OWNER, z0: 0, frame: 7, begin: BEGIN,
        tail: TAIL, target, words: 64, current: if kind == 6 { BEGIN } else { TAIL }, z1: 0, z2: 0, z3: 0 }
}
impl Ev {
    pub fn encode(&self, b: &mut Buf) {
        b.u64(self.seq).u64(self.wrap).u64(self.t0).u64(self.t1).u32(self.owner).u16(self.kind).u16(self.flags)
            .u32(self.frame_owner).u32(self.z0).u64(self.frame).u32(self.begin).u32(self.tail).u32(self.target)
            .u32(self.words).u32(self.current).u32(self.z1).u64(self.z2).u64(self.z3);
    }
}
pub fn events(evs: &[Ev]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(evs.len() as u32).u32(0);
    for e in evs {
        e.encode(&mut b);
    }
    chunk(32, b.0)
}

/// One wrap: seq0..seq0+5, kinds 1..6; phase 1 takes p1 ticks, phase 2 p2 ticks, from tb t.
pub fn wrap(seq0: u64, id: u64, t: u64, p1: u64, p2: u64) -> Vec<Ev> {
    vec![
        ev(seq0, id, 1, t, t),
        ev(seq0 + 1, id, 2, t + 1, t + 2),
        ev(seq0 + 2, id, 3, t + 2 + p1, t + 3 + p1),
        ev(seq0 + 3, id, 4, t + 4 + p1, t + 5 + p1),
        ev(seq0 + 4, id, 5, t + 5 + p1 + p2, t + 6 + p1 + p2),
        ev(seq0 + 5, id, 6, t + 7 + p1 + p2, t + 7 + p1 + p2),
    ]
}

#[derive(Clone, Copy, Default, Debug)]
pub struct St {
    pub schema: u16,
    pub flags: u16,
    pub instance: u32,
    pub attempted: u64,
    pub written: u64,
    pub dropped: u64,
    pub refused: u64,
    pub last: u64,
    pub started: u64,
    pub completed: u64,
    pub open: u64,
    pub hw: u64,
}
impl St {
    pub fn new(flags: u16, attempted: u64, written: u64, dropped: u64, last: u64, started: u64, completed: u64,
        open: u64) -> Self {
        Self { schema: 1, flags, instance: 1, attempted, written, dropped, refused: 0, last, started, completed, open,
            hw: written.min(1024) }
    }
    pub fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        b.u16(self.schema).u16(self.flags).u32(self.instance).u64(self.attempted).u64(self.written).u64(self.dropped)
            .u64(self.refused).u64(self.last).u64(self.started).u64(self.completed).u64(self.open).u64(self.hw);
        chunk(33, b.0)
    }
}

/// HEADER, CLOCKS, THREADS (owner incarnation 1), the RSX domain, CONFIG: the shortest valid bit-6 prefix.
pub fn base(flags: u32) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr { max_threads: 64, ..Default::default() }.payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, FREQ, 0)),
        chunk(T_THREADS, threads_payload(&[thr(OWNER)])),
        rsx_domain(D),
        Cfg::default().chunk(),
    ]);
    f.flags = flags;
    f
}
pub fn with(chunks: Vec<Chunk>) -> File {
    let mut f = base(65);
    f.chunks.extend(chunks);
    f
}

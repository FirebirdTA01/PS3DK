//! SPU ticks fixtures (spu-ticks slice design r1 + delta r2), written with the test-side writer in `common` (no reader
//! code).  The including crate declares `mod common;` at its root.
//!
//! [`Cap`] appends chunks in file order.  Low-level calls write one chunk each; [`Cap::walk`] emulates one drain walk
//! of the runtime contract (delta D3.2-D3.4): events and LOSS markers are closed into chunks, a CAL slot answers the
//! lane's outstanding request (its bracket's tb1 is the walk's tb, its events_before the cursor at the slot), and the
//! DRAINED + DRAIN_TB pair is written before each EPOCH and at the walk's end when the cursor advanced.
#![allow(dead_code)]

use crate::common::*;
use ps3dk_prof::profile::{FieldRule, TableEntry};
use ps3dk_prof::spu_ticks::{Iso, SpuEntry, SPU_TICKS_PREMISES};

pub const T_PROFILE: u32 = 17;
pub const T_DOMAIN: u32 = 8;
pub const T_EPOCH: u32 = 9;
pub const T_CAL: u32 = 10;
pub const T_IMAGES: u32 = 11;
pub const T_LANES: u32 = 12;
pub const T_SPU: u32 = 13;
pub const T_LOSS: u32 = 15;
pub const T_FINAL: u32 = 16;
pub const T_ORDER: u32 = 18;
pub const FREQ: u64 = 79_800_000;
pub const DIGEST: [u8; 32] = [9; 32];
/// Image descriptors: 0 spu.work, 1 spu.ping, 2 spu.other.
pub const D_WORK: u16 = 0;
pub const D_PING: u16 = 1;
pub const D_OTHER: u16 = 2;
pub const BEGIN: u8 = 1;
pub const END: u8 = 2;
pub const MARK: u8 = 3;

pub fn ext_cfg_bytes() -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(40).u32(4).u32(256).u32(65536).u64(16 << 20).u32(0).u32(0).u32(0).u32(0);
    b.0
}

/// The PROFILE payload: id, premise bits, kinds; the field values the test table accepts.
#[derive(Debug, Clone)]
pub struct Prof {
    pub id: Vec<u8>,
    pub digest: [u8; 32],
    pub premises: u32,
    pub kinds: u32,
}

impl Default for Prof {
    fn default() -> Prof {
        Prof { id: b"SPU-TEST".to_vec(), digest: DIGEST, premises: SPU_TICKS_PREMISES, kinds: 2 }
    }
}

impl Prof {
    pub fn payload(&self) -> Vec<u8> {
        let mut b = Buf::default();
        b.u16(1).u16(0).u8(self.id.len() as u8);
        b.0.extend_from_slice(&self.id);
        b.u32(1);
        b.0.extend_from_slice(&self.digest);
        b.u32(self.premises).u32(self.kinds).u16(7);
        let f = |b: &mut Buf, id: u16, st: u8, v: &[u8]| {
            b.u16(id).u8(st).u8(v.len() as u8);
            b.0.extend_from_slice(v);
        };
        f(&mut b, 1, 3, &[0; 4]);
        f(&mut b, 2, 3, &[0; 4]);
        f(&mut b, 3, 1, &FREQ.to_le_bytes());
        f(&mut b, 4, 3, &[0; 4]);
        f(&mut b, 5, 2, &[1]);
        f(&mut b, 6, 2, &[0]);
        f(&mut b, 7, 1, &ext_cfg_bytes());
        b.0
    }
}

/// The SPU ticks entry that accepts the default record exactly, with an ISO annotation.
pub fn test_table() -> Vec<SpuEntry> {
    test_table_iso(Some(Iso { lo: (999_999_888, 1_000_000_000), hi: (1_000_000_113, 1_000_000_000), note: "test".into() }))
}

pub fn test_table_iso(iso: Option<Iso>) -> Vec<SpuEntry> {
    vec![SpuEntry {
        entry: TableEntry {
            profile_id: "SPU-TEST".into(),
            kinds: 2,
            premise_bits: SPU_TICKS_PREMISES,
            digests: vec![DIGEST],
            fields: vec![
                (1, FieldRule::Any("no query in the SDK".into())),
                (2, FieldRule::Any("no non-identifying query".into())),
                (3, FieldRule::RequiredMatch(vec![FREQ.to_le_bytes().to_vec()])),
                (4, FieldRule::Any("not readable".into())),
                (5, FieldRule::RequiredMatch(vec![vec![0], vec![1]])),
                (6, FieldRule::RequiredMatch(vec![vec![0]])),
                (7, FieldRule::RequiredMatch(vec![ext_cfg_bytes()])),
            ],
            evidence: vec!["test".into()],
        },
        rmax: 2,
        iso,
    }]
}

// ---- order records --------------------------------------------------------------------------------------------

pub fn rec_bracket_at(lane: u32, epoch: u32, bseq: u32, before: u64) -> [u8; 24] {
    let mut b = Buf::default();
    b.u32(1).u32(lane).u32(epoch).u32(bseq).u64(before);
    b.0.try_into().unwrap()
}
pub fn rec_drained(lane: u32, epoch: u32, through: u64) -> [u8; 24] {
    let mut b = Buf::default();
    b.u32(2).u32(lane).u32(epoch).u32(0).u64(through);
    b.0.try_into().unwrap()
}
pub fn rec_drain_tb(lane: u32, tb: u64, through: u64) -> [u8; 24] {
    let mut b = Buf::default();
    b.u32(3).u32(lane).u64(tb).u64(through);
    b.0.try_into().unwrap()
}
pub fn rec_created(lane: u32, tb: u64) -> [u8; 24] {
    let mut b = Buf::default();
    b.u32(4).u32(lane).u64(tb).u64(0);
    b.0.try_into().unwrap()
}
pub fn rec_final(lane: u32, emitted: u64, dropped: u64) -> [u8; 24] {
    let mut b = Buf::default();
    b.u32(5).u32(lane).u64(emitted).u64(dropped);
    b.0.try_into().unwrap()
}
pub fn order_chunk(recs: &[[u8; 24]]) -> Chunk {
    let mut b = Buf::default();
    b.u32(recs.len() as u32).u32(0);
    for r in recs {
        b.0.extend_from_slice(r);
    }
    chunk(T_ORDER, b.0)
}

// ---- the capture builder --------------------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct LaneB {
    pub id: u32,
    pub domain: u32,
    pub epoch: u32,
    pub next: u64,
    pub events: u64,
    pub lost: u64,
    pub brackets_epoch: u64,
    /// Accepted order records of kinds 1-4.
    pub order: u64,
    pub dropped: u64,
    pub order_through: u64,
    /// The outstanding request (tb0) of the current epoch.
    pub request: Option<u64>,
    pub depth: i64,
    pub unknown: bool,
}

/// One item a walk loads, in ring order.
#[derive(Debug, Clone, Copy)]
pub enum Item {
    /// (raw, desc, kind, depth)
    Ev(u32, u16, u8, u8),
    Loss(u64),
    /// the CAL answer's raw read (answers the outstanding request)
    Cal(u32),
    /// an EPOCH slot (reason 2) with its written value
    Epoch(u32),
}

#[derive(Debug, Clone)]
pub struct Cap {
    pub ext: Vec<Chunk>,
    pub lanes: Vec<LaneB>,
    pub profile: Option<Prof>,
    pub flags: u32,
    /// Drop (do not write) the next k DRAINED+DRAIN_TB pairs, counting them dropped (k = 2 records each).
    pub drop_pairs: u32,
    /// Drop the next k BRACKET_AT records, counting each dropped.
    pub drop_brackets_at: u32,
}

impl Cap {
    /// `n` lanes (ids 1..=n, domains 2..=n+1) declared at once, each with LANE_CREATED at `created_tb`.
    pub fn new(n: u32, created_tb: u64) -> Cap {
        let mut c = Cap::empty();
        c.image();
        c.declare(&(1..=n).collect::<Vec<u32>>(), created_tb);
        c
    }

    /// No chunk yet.
    pub fn empty() -> Cap {
        Cap { ext: Vec::new(), lanes: Vec::new(), profile: Some(Prof::default()), flags: 7, drop_pairs: 0, drop_brackets_at: 0 }
    }

    /// The one SPU image: descriptors 0 spu.work, 1 spu.ping, 2 spu.other.
    pub fn image(&mut self) {
        let mut b = Buf::default();
        b.u32(1).u32(0).bytes_str(b"spu-build").u32(3);
        for (i, nm) in ["spu.work", "spu.ping", "spu.other"].iter().enumerate() {
            b.u32(i as u32).u32(0x100 + i as u32).u32(10 + i as u32).str(nm).str("spu/main.c");
        }
        self.ext.push(chunk(T_IMAGES, b.0));
    }

    /// DOMAIN then SPU_LANES (at most 64 entries per chunk), then LANE_CREATED per lane, as the runtime's declare.
    pub fn declare(&mut self, ids: &[u32], created_tb: u64) {
        for g in ids.chunks(64) {
            let mut b = Buf::default();
            b.u32(g.len() as u32);
            for &l in g {
                b.u32(l + 1).u8(1).u8(32).u8(1).u8(0).u32(1).u32(l).u64(0).u64(0).u64(0).u64(0).str("spu-dec");
            }
            self.ext.push(chunk(T_DOMAIN, b.0));
        }
        for g in ids.chunks(64) {
            let mut b = Buf::default();
            b.u32(g.len() as u32);
            for &l in g {
                b.u32(l).u32(l + 1).u32(0).u32(0).u64(u64::from(l)).str("worker");
            }
            self.ext.push(chunk(T_LANES, b.0));
        }
        for &l in ids {
            self.lanes.push(LaneB { id: l, domain: l + 1, order: 1, ..LaneB::default() });
            self.ext.push(order_chunk(&[rec_created(l, created_tb)]));
        }
    }

    pub fn lane(&mut self, id: u32) -> &mut LaneB {
        self.lanes.iter_mut().find(|l| l.id == id).expect("lane")
    }

    /// An EPOCH chunk for the lane (reason 1 for the first, 2 after): `tb` is the drain's own read.
    pub fn epoch(&mut self, id: u32, written: u32, tb: u64) {
        let l = self.lane(id);
        let e = l.epoch + 1;
        let (reason, wv) = if e == 1 { (1u8, 0u64) } else { (2u8, u64::from(written)) };
        if e > 1 && (l.depth != 0 || l.unknown) {
            l.unknown = true;
        }
        l.epoch = e;
        l.brackets_epoch = 0;
        l.request = None;
        let d = l.domain;
        let mut b = Buf::default();
        b.u32(1).u32(d).u32(e).u8(reason).u8(0).u8(0).u8(0).u64(wv).u64(tb);
        self.ext.push(chunk(T_EPOCH, b.0));
    }

    /// One SPU_EVENTS chunk of the current epoch.
    pub fn events(&mut self, id: u32, ev: &[(u32, u16, u8, u8)]) {
        let l = self.lane(id);
        let mut b = Buf::default();
        b.u32(l.id).u32(l.epoch).u64(l.next).u32(ev.len() as u32);
        for &(raw, desc, kind, depth) in ev {
            b.u32(raw).u16(desc).u8(kind).u8(depth);
            match kind {
                BEGIN => l.depth += 1,
                END => l.depth -= 1,
                _ => {}
            }
        }
        l.next += ev.len() as u64;
        l.events += ev.len() as u64;
        self.ext.push(chunk(T_SPU, b.0));
    }

    pub fn loss(&mut self, id: u32, count: u64) {
        let l = self.lane(id);
        let mut b = Buf::default();
        b.u32(l.id).u8(1).u8(1).u16(0).u64(l.next).u64(count).u32(0);
        l.next += count;
        l.lost += count;
        l.unknown = true;
        l.depth = 0;
        self.ext.push(chunk(T_LOSS, b.0));
    }

    /// A CALIBRATION bracket (the next of the epoch), then its BRACKET_AT when `before` is given.
    pub fn bracket(&mut self, id: u32, raw: u32, tb0: u64, tb1: u64, before: Option<u64>) {
        let l = self.lane(id);
        let (d, e, s) = (l.domain, l.epoch, l.brackets_epoch);
        l.brackets_epoch += 1;
        let mut b = Buf::default();
        b.u32(d).u32(e).u64(s).u32(1).u64(u64::from(raw)).u64(tb0).u64(tb1);
        self.ext.push(chunk(T_CAL, b.0));
        if let Some(k) = before {
            if self.drop_brackets_at > 0 {
                self.drop_brackets_at -= 1;
                self.lane(id).dropped += 1;
            } else {
                self.lane(id).order += 1;
                self.ext.push(order_chunk(&[rec_bracket_at(id, e, s as u32, k)]));
            }
        }
    }

    /// DRAINED + DRAIN_TB at the cursor, in the current epoch (only when the cursor advanced, as the runtime does).
    pub fn drained(&mut self, id: u32, tb: u64) {
        let l = self.lane(id);
        if l.next <= l.order_through {
            return;
        }
        l.order_through = l.next;
        let (e, t) = (l.epoch, l.next);
        if self.drop_pairs > 0 {
            self.drop_pairs -= 1;
            self.lane(id).dropped += 2;
            return;
        }
        self.lane(id).order += 2;
        self.ext.push(order_chunk(&[rec_drained(id, e, t), rec_drain_tb(id, tb, t)]));
    }

    /// A request issued by the drain at `tb0` (the next CAL answers it).
    pub fn request(&mut self, id: u32, tb0: u64) {
        self.lane(id).request = Some(tb0);
    }

    /// One drain walk at `tb` (the walk's tb1) loading `items` in ring order (delta D3.0, D3.2).
    pub fn walk(&mut self, id: u32, tb: u64, items: &[Item]) {
        let mut evs: Vec<(u32, u16, u8, u8)> = Vec::new();
        let mut pending_loss = 0u64;
        let mut br: Option<(u32, u64, u64)> = None; // (raw, tb0, events_before)
        let close = |c: &mut Cap, evs: &mut Vec<(u32, u16, u8, u8)>, pl: &mut u64| {
            if !evs.is_empty() {
                c.events(id, evs);
                evs.clear();
            }
            if *pl > 0 {
                c.loss(id, *pl);
                *pl = 0;
            }
        };
        for it in items {
            match *it {
                Item::Ev(r, d, k, dp) => {
                    if pending_loss > 0 {
                        self.loss(id, pending_loss);
                        pending_loss = 0;
                    }
                    evs.push((r, d, k, dp));
                }
                Item::Loss(n) => {
                    if !evs.is_empty() {
                        self.events(id, &evs);
                        evs.clear();
                    }
                    pending_loss += n;
                }
                Item::Cal(r) => {
                    let l = self.lane(id);
                    let tb0 = l.request.take().expect("an outstanding request");
                    let before = l.next + evs.len() as u64 + pending_loss;
                    br = Some((r, tb0, before));
                }
                Item::Epoch(wv) => {
                    close(self, &mut evs, &mut pending_loss);
                    if let Some((r, t0, k)) = br.take() {
                        self.bracket(id, r, t0, tb, Some(k));
                    }
                    self.drained(id, tb);
                    self.epoch(id, wv, tb + 1);
                }
            }
        }
        close(self, &mut evs, &mut pending_loss);
        if let Some((r, t0, k)) = br.take() {
            self.bracket(id, r, t0, tb, Some(k));
        }
        self.drained(id, tb);
    }

    /// ORDER_FINAL (records_emitted = the lane's records of kinds 1-4) then the lane's DOMAIN_FINAL.
    pub fn finish(&mut self, id: u32) {
        let l = self.lane(id).clone();
        self.ext.push(order_chunk(&[rec_final(id, l.order, l.dropped)]));
        self.domain_final(id);
    }

    /// The lane's DOMAIN_FINAL only.
    pub fn domain_final(&mut self, id: u32) {
        let l = self.lane(id).clone();
        let open = if l.unknown { u32::MAX } else { l.depth as u32 };
        let mut b = Buf::default();
        b.u32(1).u32(id).u8(1).u8(1).u16(0).u64(l.next).u64(l.events).u64(l.lost).u32(open);
        self.ext.push(chunk(T_FINAL, b.0));
    }

    pub fn finish_all(&mut self) {
        let ids: Vec<u32> = self.lanes.iter().map(|l| l.id).collect();
        for id in ids {
            self.finish(id);
        }
    }

    pub fn file(&self) -> File {
        let mut f = scenario(vec![lane(1, seqd(0, vec![frame(0, 100, 0), frame(0, 200, 1)]))]).file();
        let at = f.find(T_TRAILER);
        for (i, c) in self.ext.iter().enumerate() {
            f.chunks.insert(at + i, c.clone());
        }
        if let Some(p) = &self.profile {
            f.chunks.insert(2, chunk(T_PROFILE, p.payload()));
        }
        // the CLOCKS chunk carries FREQ (the PROFILE's TB_FREQ_HZ must equal it)
        f.chunks[1] = chunk(T_CLOCKS, clocks_payload(1, 1, 0, FREQ, 1000));
        f.flags = self.flags;
        f
    }

    pub fn bytes(&self) -> Vec<u8> {
        self.file().bytes()
    }

    /// The file without its TRAILER and with every chunk from index `from` (of `ext`) on removed: a torn file.
    pub fn torn_after(&self, keep_ext: usize) -> Vec<u8> {
        let mut c = self.clone();
        c.ext.truncate(keep_ext);
        c.file().without_trailer().bytes()
    }
}

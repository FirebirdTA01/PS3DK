//! PROFILE and RSX-MEASURED fixtures (profile-format-spec.md, rsx-measured-slice-design.md), written with the
//! test-side writer in `common` (no reader code).  The including crate declares `mod common;` at its root.
#![allow(dead_code)]

use crate::common::*;
use ps3dk_prof::profile::{FieldRule, TableEntry};

pub const T_PROFILE: u32 = 17;
pub const T_DOMAIN: u32 = 8;
pub const T_EPOCH: u32 = 9;
pub const T_CAL: u32 = 10;
pub const T_GPU: u32 = 14;
pub const T_LOSS: u32 = 15;
pub const T_FINAL: u32 = 16;
pub const RSX_D: u32 = 3;
pub const OWNER: u32 = 1;
pub const FREQ: u64 = 79_800_000;
/// PPU descriptors: 0 render.submit, 1 render.flip_wait, 2 other; GPU (scope 2): 3 rsx.setup, 4 rsx.draw.
pub const D_SUBMIT: u32 = 0;
pub const D_WAIT: u32 = 1;
pub const D_OTHER: u32 = 2;
pub const D_SETUP: u32 = 3;
pub const D_DRAW: u32 = 4;
pub const RSX_PREMISES: u32 = 0b1_1100_1111; // C-PPU C-RSX A S R64 E C-PPU-Z
pub const DIGEST: [u8; 32] = [7; 32];

// ---- the PROFILE payload ---------------------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct PField {
    pub id: u16,
    pub status: u8,
    pub width: u8,
    pub value: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct Prof {
    pub version: u16,
    pub reserved0: u16,
    pub id: Vec<u8>,
    pub id_len: Option<u8>,
    pub alg: u32,
    pub digest: [u8; 32],
    pub premises: u32,
    pub kinds: u32,
    pub count: Option<u16>,
    pub fields: Vec<PField>,
    pub tail: Vec<u8>,
}

pub fn ext_cfg_bytes() -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(40).u32(0).u32(0).u32(0).u64(16 << 20).u32(1).u32(256).u32(0).u32(0);
    b.0
}

impl Default for Prof {
    fn default() -> Prof {
        let f = |id: u16, status: u8, width: u8, value: Vec<u8>| PField { id, status, width, value };
        Prof {
            version: 1,
            reserved0: 0,
            id: b"RSX-TEST".to_vec(),
            id_len: None,
            alg: 1,
            digest: DIGEST,
            premises: RSX_PREMISES,
            kinds: 1,
            count: None,
            fields: vec![
                f(1, 3, 4, vec![0; 4]),
                f(2, 3, 4, vec![0; 4]),
                f(3, 1, 8, FREQ.to_le_bytes().to_vec()),
                f(4, 3, 4, vec![0; 4]),
                f(5, 2, 1, vec![1]),
                f(6, 2, 1, vec![0]),
                f(7, 1, 40, ext_cfg_bytes()),
            ],
            tail: Vec::new(),
        }
    }
}

impl Prof {
    pub fn payload(&self) -> Vec<u8> {
        let mut b = Buf::default();
        b.u16(self.version).u16(self.reserved0).u8(self.id_len.unwrap_or(self.id.len() as u8));
        b.0.extend_from_slice(&self.id);
        b.u32(self.alg);
        b.0.extend_from_slice(&self.digest);
        b.u32(self.premises).u32(self.kinds).u16(self.count.unwrap_or(self.fields.len() as u16));
        for f in &self.fields {
            b.u16(f.id).u8(f.status).u8(f.width);
            b.0.extend_from_slice(&f.value);
        }
        b.0.extend_from_slice(&self.tail);
        b.0
    }
}

/// The table entry that accepts the default record exactly (HW_MODEL_CLASS, SYS_SW_VERSION, SPU_USABLE: ANY).
pub fn test_table() -> Vec<TableEntry> {
    vec![TableEntry {
        profile_id: "RSX-TEST".into(),
        kinds: 1,
        premise_bits: RSX_PREMISES,
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
    }]
}

// ---- extension chunks ------------------------------------------------------------------------------------------

pub fn rsx_domain() -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(RSX_D).u8(2).u8(64).u8(2).u8(0).u32(2).u32(0).u64(0).u64(0).u64(0).u64(0).str("rsx");
    chunk(T_DOMAIN, b.0)
}
pub fn epoch1() -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(RSX_D).u32(1).u8(1).u8(0).u8(0).u8(0).u64(0).u64(5000);
    chunk(T_EPOCH, b.0)
}
/// A later epoch (reason 5) of the RSX domain.
pub fn epoch_k(k: u32, tb: u64) -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(RSX_D).u32(k).u8(5).u8(0).u8(0).u8(0).u64(0).u64(tb);
    chunk(T_EPOCH, b.0)
}
pub fn calibration(brackets: &[(u64, u64, u64)]) -> Chunk {
    calibration_e(1, brackets)
}
pub fn calibration_e(epoch: u32, brackets: &[(u64, u64, u64)]) -> Chunk {
    let mut b = Buf::default();
    b.u32(RSX_D).u32(epoch).u64(0).u32(brackets.len() as u32);
    for &(f, t0, t1) in brackets {
        b.u64(f).u64(t0).u64(t1);
    }
    chunk(T_CAL, b.0)
}
/// (raw, desc, kind 1 BEGIN / 2 END, depth, owner, frame)
pub fn gpu_events(first: u64, ev: &[(u64, u32, u8, u8, u32, u64)]) -> Chunk {
    gpu_events_e(1, first, ev)
}
pub fn gpu_events_e(epoch: u32, first: u64, ev: &[(u64, u32, u8, u8, u32, u64)]) -> Chunk {
    let mut b = Buf::default();
    b.u32(RSX_D).u32(epoch).u64(first).u32(ev.len() as u32);
    for &(raw, desc, kind, depth, owner, frame) in ev {
        b.u64(raw).u32(desc).u8(kind).u8(depth).u16(0).u32(owner).u64(frame);
    }
    chunk(T_GPU, b.0)
}
pub fn final_(next: u64, events: u64, lost: u64, open: u32) -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(RSX_D).u8(2).u8(1).u16(0).u64(next).u64(events).u64(lost).u32(open);
    chunk(T_FINAL, b.0)
}
pub fn loss(first: u64, count: u64) -> Chunk {
    let mut b = Buf::default();
    b.u32(RSX_D).u8(2).u8(1).u16(0).u64(first).u64(count).u32(0);
    chunk(T_LOSS, b.0)
}

// ---- the RSX scenario ----------------------------------------------------------------------------------------------

/// One frame of the PPU owner: its FRAME tb and the convention's zones (None: omitted), plus extra zones.
#[derive(Debug, Clone)]
pub struct PpuFrame {
    pub tb: u64,
    pub submit: Vec<(u64, u64)>,
    pub wait: Vec<(u64, u64)>,
    /// The one wait zone nested inside the one submit zone (END(submit) > BEGIN(wait)).
    pub nested: bool,
}

#[derive(Debug, Clone)]
pub struct Rsx {
    pub frames: Vec<PpuFrame>,
    /// The tb of the final FRAME (closes the last measured frame).
    pub last_frame_tb: u64,
    pub brackets: Vec<(u64, u64, u64)>,
    /// GPU events, written in this order with consecutive seqs.
    pub events: Vec<(u64, u32, u8, u8, u32, u64)>,
    pub gap_after_frame: Option<usize>,
    pub profile: Option<Prof>,
    pub flags: u32,
    pub extra_chunks_after_profile: Vec<Chunk>,
    pub loss: Option<(u64, u64)>,
    /// BEGINs open at the FINAL (the FINAL's open_begins).
    pub final_open: u32,
    /// PPU owner lanes 1..=owners, each with the same frame records.
    pub owners: u32,
    /// Epochs 2.. of the RSX domain: (EPOCH tb, brackets, events), written after epoch 1's.
    pub more_epochs: Vec<(u64, Vec<(u64, u64, u64)>, Vec<(u64, u32, u8, u8, u32, u64)>)>,
}

/// The baseline: 8 frames 10 000 ticks apart; true map t = f / 2; brackets every ~third of the run, +-3000 ticks
/// wide; per frame a setup pair (t + 1000..1500) and a draw pair (t + 2000..3000); submit [t+100, t+400], wait
/// [t+500, t+9000]; every endpoint interpolated, some constraints decisive.
pub fn baseline() -> Rsx {
    let n = 8u64;
    let t = |k: u64| 100_000 + 10_000 * k;
    let frames = (0..n).map(|k| PpuFrame {
            tb: t(k),
            submit: vec![(t(k) + 100, t(k) + 400)],
            wait: vec![(t(k) + 500, t(k) + 9000)],
            nested: false,
        }).collect();
    let br = |tt: u64| (2 * tt, tt - 3000, tt + 3000);
    let brackets = vec![br(100_000), br(126_000), br(153_000), br(180_000)];
    let mut events = Vec::new();
    for k in 0..n {
        let tk = t(k);
        events.push((2 * (tk + 1000), D_SETUP, 1, 0, OWNER, k));
        events.push((2 * (tk + 1500), D_SETUP, 2, 0, OWNER, k));
        events.push((2 * (tk + 2000), D_DRAW, 1, 0, OWNER, k));
        events.push((2 * (tk + 3000), D_DRAW, 2, 0, OWNER, k));
    }
    Rsx {
        frames,
        last_frame_tb: t(n),
        brackets,
        events,
        gap_after_frame: None,
        profile: Some(Prof::default()),
        flags: 3,
        extra_chunks_after_profile: Vec::new(),
        loss: None,
        final_open: 0,
        owners: 1,
        more_epochs: Vec::new(),
    }
}

/// Many owners, epochs and pairs: `owners` PPU lanes with the baseline's frame pattern over `epochs * fpe` frames;
/// per RSX epoch 4 brackets across its frames; per frame and owner a setup and a draw pair under the owner's token
/// (true map t = f / 2).  Work-bound controls only (no state is pinned by it).
pub fn many(owners: u32, epochs: u32, fpe: u64) -> Rsx {
    let t = |k: u64| 100_000 + 10_000 * k;
    let nf = u64::from(epochs) * fpe;
    let mut r = baseline();
    r.frames = (0..nf)
        .map(|k| PpuFrame { tb: t(k), submit: vec![(t(k) + 100, t(k) + 400)], wait: vec![(t(k) + 500, t(k) + 9000)], nested: false })
        .collect();
    r.last_frame_tb = t(nf);
    r.owners = owners;
    let w = 7000 / (2 * u64::from(owners));
    let mut per: Vec<(u64, Vec<(u64, u64, u64)>, Vec<(u64, u32, u8, u8, u32, u64)>)> = Vec::new();
    for e in 0..u64::from(epochs) {
        let (k0, k1) = (e * fpe, (e + 1) * fpe);
        let span = t(k1) - t(k0) - 1000;
        let br = (0..4).map(|j| {
            let tt = t(k0) + 500 + j * span / 3;
            (2 * tt, tt - 3000, tt + 3000)
        }).collect();
        let mut ev = Vec::new();
        for k in k0..k1 {
            for o in 1..=owners {
                for (j, d) in [D_SETUP, D_DRAW].into_iter().enumerate() {
                    let base = t(k) + 1000 + (u64::from(o - 1) * 2 + j as u64) * w;
                    ev.push((2 * base, d, 1, 0, o, k));
                    ev.push((2 * (base + w / 2), d, 2, 0, o, k));
                }
            }
        }
        per.push((t(k0), br, ev));
    }
    let (_, b1, e1) = per.remove(0);
    r.brackets = b1;
    r.events = e1;
    r.more_epochs = per;
    r
}

impl Rsx {
    pub fn descs() -> Vec<Desc> {
        let mut v = vec![desc(D_SUBMIT, "render.submit", ""), desc(D_WAIT, "render.flip_wait", ""), desc(D_OTHER, "other", "")];
        for (i, n) in [(D_SETUP, "rsx.setup"), (D_DRAW, "rsx.draw")] {
            let mut d = desc(i, n, "");
            d.scope = 2;
            v.push(d);
        }
        v
    }

    fn ppu_records(&self) -> Vec<Rec> {
        let mut recs = Vec::new();
        let mut span = 1u64;
        for (k, f) in self.frames.iter().enumerate() {
            recs.push(frame(0, f.tb, k as u64));
            if f.nested {
                let ((sb, se), (wb, we)) = (f.submit[0], f.wait[0]);
                recs.extend(zone(0, D_SUBMIT, 0, sb, span, 0));
                recs.extend(zone(0, D_WAIT, 1, wb, span + 1, span));
                recs.push(end(0, D_WAIT, 1, we, span + 1));
                recs.push(end(0, D_SUBMIT, 0, se, span));
                span += 2;
                continue;
            }
            let mut zones: Vec<(u64, u64, u32)> = Vec::new();
            for &(b, e) in &f.submit {
                zones.push((b, e, D_SUBMIT));
            }
            for &(b, e) in &f.wait {
                zones.push((b, e, D_WAIT));
            }
            zones.sort();
            for (b, e, d) in zones {
                recs.extend(zone(0, d, 0, b, span, 0));
                recs.push(end(0, d, 0, e, span));
                span += 1;
            }
            if self.gap_after_frame == Some(k) {
                recs.push(gap(0, 0)); // placeholder, renumbered below
            }
        }
        recs.push(frame(0, self.last_frame_tb, self.frames.len() as u64));
        // sequences: consecutive, a GAP takes two lost seqs
        let mut next = 0u64;
        let mut out = Vec::new();
        for mut r in recs {
            if r.kind == 5 {
                r = gap(next, next + 1);
                next += 2;
            } else {
                r.seq = next;
                next += 1;
            }
            out.push(r);
        }
        out
    }

    pub fn file(&self) -> File {
        let recs = self.ppu_records();
        let mut s = scenario((1..=self.owners.max(1)).map(|o| lane(o, recs.clone())).collect());
        s.descs = Self::descs();
        let mut f = s.file();
        let at = f.find(T_TRAILER);
        let mut n = self.events.len() as u64;
        let mut ext = vec![rsx_domain(), epoch1(), calibration(&self.brackets)];
        if !self.events.is_empty() {
            ext.push(gpu_events(0, &self.events));
        }
        for (i, (tb, br, ev)) in self.more_epochs.iter().enumerate() {
            let k = i as u32 + 2;
            ext.push(epoch_k(k, *tb));
            ext.push(calibration_e(k, br));
            if !ev.is_empty() {
                ext.push(gpu_events_e(k, n, ev));
                n += ev.len() as u64;
            }
        }
        if let Some((first, count)) = self.loss {
            ext.push(loss(first, count));
        }
        let lost = self.loss.map_or(0, |l| l.1);
        ext.push(final_(n + lost, n, lost, self.final_open));
        for (i, c) in ext.into_iter().enumerate() {
            f.chunks.insert(at + i, c);
        }
        if let Some(p) = &self.profile {
            f.chunks.insert(2, chunk(T_PROFILE, p.payload()));
        }
        for (i, c) in self.extra_chunks_after_profile.iter().enumerate() {
            f.chunks.insert(3 + i, c.clone());
        }
        f.flags = self.flags;
        f
    }

    pub fn bytes(&self) -> Vec<u8> {
        self.file().bytes()
    }
}

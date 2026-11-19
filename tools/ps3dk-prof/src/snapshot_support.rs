//! Saved-frame support: strict selector and original-source diagnostic provenance.
//! No aggregate in this record supplies a bar, duration or positive clock qualification.
use crate::model::{malformed, Capture, Malformed, Rule};
use std::collections::BTreeMap;

pub const CHUNK: u32 = 49;
pub const EXPORT_FLAG: u16 = 4;
pub const HEADER: usize = 160;
pub const MAX_BYTES: usize = 64 << 20;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LaneFrames {
    pub first: u64,
    pub last: u64,
    pub count: u64,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Diagnostic {
    pub kind: u16,
    pub source_kind: u16,
    pub source: u32,
    pub key: u64,
    pub value: u64,
    pub auxiliary: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Selection {
    pub flags: u16,
    pub owner: u32,
    pub frame: u64,
    pub tb: u64,
    pub next_tb: u64,
    pub source_bytes: u64,
    pub source_sha: [u8; 32],
    pub support_sha: [u8; 32],
    pub semantic_sha: [u8; 32],
    pub clock_bytes: u64,
    pub auto_rows: u64,
    pub lanes: BTreeMap<u32, LaneFrames>,
    pub diagnostics: Vec<Diagnostic>,
}
fn bad<T>(s: impl Into<String>) -> Result<T, Malformed> {
    malformed(Rule::Export, format!("SNAPSHOT_SUPPORT: {}", s.into()))
}
fn u16at(p: &[u8], n: usize) -> u16 {
    u16::from_le_bytes(p[n..n + 2].try_into().unwrap())
}
fn u32at(p: &[u8], n: usize) -> u32 {
    u32::from_le_bytes(p[n..n + 4].try_into().unwrap())
}
fn u64at(p: &[u8], n: usize) -> u64 {
    u64::from_le_bytes(p[n..n + 8].try_into().unwrap())
}

impl Selection {
    pub fn parse(p: &[u8]) -> Result<Self, Malformed> {
        if p.len() < HEADER || p.len() > MAX_BYTES {
            return bad("header length/cap");
        }
        if u16at(p, 0) != 1 || u16at(p, 2) & !3 != 0 {
            return bad("schema/premise flags");
        }
        let (nl, nd) = (u32at(p, 104) as usize, u32at(p, 108) as usize);
        if nl > 65535 || nd > 4 * (nl + 1) || p.len() != HEADER + (nl + nd) * 32 {
            return bad("table length/count");
        }
        let mut lanes = BTreeMap::new();
        let mut previous = None;
        for row in p[HEADER..HEADER + nl * 32].chunks_exact(32) {
            let id = u32at(row, 0);
            let f = LaneFrames {
                first: u64at(row, 8),
                last: u64at(row, 16),
                count: u64at(row, 24),
            };
            if id == 0
                || previous.is_some_and(|v| id <= v)
                || u32at(row, 4) != 0
                || if f.count == 0 {
                    f.first != 0 || f.last != 0
                } else {
                    f.first > f.last || f.count - 1 > f.last - f.first
                }
            {
                return bad("lane manifest");
            }
            previous = Some(id);
            lanes.insert(id, f);
        }
        let mut diagnostics = Vec::with_capacity(nd);
        let mut previous = None;
        for row in p[HEADER + nl * 32..].chunks_exact(32) {
            let d = Diagnostic {
                kind: u16at(row, 0),
                source_kind: u16at(row, 2),
                source: u32at(row, 4),
                key: u64at(row, 8),
                value: u64at(row, 16),
                auxiliary: u64at(row, 24),
            };
            let key = (d.kind, d.source_kind, d.source, d.key);
            let valid = match d.kind {
                1 | 2 => d.source_kind == 1 && lanes.contains_key(&d.source) && d.value <= 1,
                3 => d.source_kind == 1 && lanes.contains_key(&d.source),
                4 => d.source_kind == 4 && d.source == 0,
                5 => d.source_kind == 0 && d.source == 0,
                6 => {
                    d.source_kind == 0
                        && d.source == 0
                        && d.value <= MAX_BYTES as u64
                        && d.value % 32 == 0
                }
                _ => false,
            };
            if !valid || d.key != 0 || d.auxiliary != 0 || previous.is_some_and(|v| key <= v) {
                return bad("diagnostic kind/source/key/value");
            }
            previous = Some(key);
            diagnostics.push(d);
        }
        let out = Self {
            flags: u16at(p, 2),
            owner: u32at(p, 4),
            frame: u64at(p, 8),
            tb: u64at(p, 16),
            next_tb: u64at(p, 24),
            source_bytes: u64at(p, 32),
            source_sha: p[40..72].try_into().unwrap(),
            support_sha: p[72..104].try_into().unwrap(),
            semantic_sha: p[128..160].try_into().unwrap(),
            clock_bytes: u64at(p, 112),
            auto_rows: u64at(p, 120),
            lanes,
            diagnostics,
        };
        if out.source_bytes < 16 || out.next_tb <= out.tb || !out.lanes.contains_key(&out.owner) {
            return bad("selector/source bounds");
        }
        Ok(out)
    }
    pub fn encode(&self) -> Vec<u8> {
        let mut p = vec![0; HEADER];
        p[..2].copy_from_slice(&1u16.to_le_bytes());
        p[2..4].copy_from_slice(&self.flags.to_le_bytes());
        p[4..8].copy_from_slice(&self.owner.to_le_bytes());
        for (n, v) in [
            (8, self.frame),
            (16, self.tb),
            (24, self.next_tb),
            (32, self.source_bytes),
            (112, self.clock_bytes),
            (120, self.auto_rows),
        ] {
            p[n..n + 8].copy_from_slice(&v.to_le_bytes());
        }
        p[40..72].copy_from_slice(&self.source_sha);
        p[72..104].copy_from_slice(&self.support_sha);
        p[128..160].copy_from_slice(&self.semantic_sha);
        p[104..108].copy_from_slice(&(self.lanes.len() as u32).to_le_bytes());
        p[108..112].copy_from_slice(&(self.diagnostics.len() as u32).to_le_bytes());
        for (&id, f) in &self.lanes {
            p.extend(id.to_le_bytes());
            p.extend(0u32.to_le_bytes());
            for n in [f.first, f.last, f.count] {
                p.extend(n.to_le_bytes());
            }
        }
        for d in &self.diagnostics {
            p.extend(d.kind.to_le_bytes());
            p.extend(d.source_kind.to_le_bytes());
            p.extend(d.source.to_le_bytes());
            for n in [d.key, d.value, d.auxiliary] {
                p.extend(n.to_le_bytes());
            }
        }
        p
    }
    pub fn diagnostic(&self, kind: u16, source: u32) -> Option<u64> {
        self.diagnostics
            .iter()
            .find(|d| d.kind == kind && d.source == source)
            .map(|d| d.value)
    }
    pub(crate) fn validate(&self, cap: &Capture) -> Result<(), Malformed> {
        if cap.export.as_ref().is_none_or(|e| {
            e.source_size != self.source_bytes
                || e.frame_first != self.frame
                || e.frame_last != self.frame
        }) {
            return bad("source length/selected frame differs from EXPORT");
        }
        if self.lanes.len() != cap.lanes.len() {
            return bad("lane count differs from declarations");
        }
        for (&id, lane) in &cap.lanes {
            let got = LaneFrames {
                first: lane.frames.first().map_or(0, |f| f.id),
                last: lane.frames.last().map_or(0, |f| f.id),
                count: lane.frames.len() as u64,
            };
            if self.lanes.get(&id) != Some(&got) {
                return bad(format!("lane {id} frame manifest differs"));
            }
            if self.diagnostic(1, id) == Some(1)
                && (!lane.spans.is_empty() || lane.clock_anomaly || lane.gap_count != 0)
            {
                return bad("frame-only source contradicts retained evidence");
            }
            if self.diagnostic(2, id) == Some(0) && lane.clock_anomaly {
                return bad("source clock anomaly erased");
            }
            if self.diagnostic(3, id).is_some_and(|v| v < lane.gap_count) {
                return bad("source GAP count below retained count");
            }
            let Some(e) = cap.export.as_ref().and_then(|e| e.lanes.get(&id)) else {
                return bad("missing EXPORT lane");
            };
            if lane.written != e.kept
                || lane.first_seq.unwrap_or(0) != e.first_seq
                || lane.last_seq.unwrap_or(0) != e.last_seq
            {
                return bad("retained record count/sequence differs from EXPORT");
            }
        }
        let lane = &cap.lanes[&self.owner];
        if !lane.frames.windows(2).any(|p| {
            p[0].id == self.frame
                && p[0].tb == self.tb
                && p[0].id.checked_add(1) == Some(p[1].id)
                && p[1].tb == self.next_tb
        }) {
            return bad("selector is not a retained consecutive frame interval");
        }
        if cap.auto.as_ref().map_or(0, |a| a.frames_parsed) != self.auto_rows {
            return bad("AUTO row count differs");
        }
        if let Some(a) = &cap.auto {
            let mut frames = std::collections::BTreeSet::from([(self.owner, self.frame)]);
            for op in &a.ops {
                frames.extend([op.token(), op.build].into_iter().flatten());
            }
            if a.frames
                .iter()
                .any(|r| !frames.contains(&(r.owner, r.frame)))
            {
                return bad("AUTO row is outside the selected execution/build support");
            }
        }
        if self.diagnostic(4, 0).is_some()
            && cap.auto.as_ref().and_then(|a| a.config.as_ref()).is_none()
        {
            return bad("AUTO diagnostic without CONFIG");
        }
        if self
            .diagnostic(4, 0)
            .is_some_and(|n| cap.auto.as_ref().is_some_and(|a| n < a.ops_dropped))
        {
            return bad("source AUTO loss below retained loss");
        }
        Ok(())
    }
}

/// Hash only the supported chunk bodies and their type/length, with checked framing.
/// CRC/sequence/placement checks remain the structural reader's responsibility.
pub fn support_digest(bytes: &[u8]) -> Result<([u8; 32], u64), Malformed> {
    if bytes.len() < 16 || bytes.len() > MAX_BYTES {
        return bad("file size/cap");
    }
    let mut h = crate::sha256::Sha256::new();
    h.update(&bytes[..16]);
    let mut pos = 16;
    let mut clocks = 0;
    while pos < bytes.len() {
        if bytes.len() - pos < 20 {
            return bad("torn support");
        }
        let ty = u32at(bytes, pos);
        let n = u32at(bytes, pos + 4) as usize;
        let end = pos
            .checked_add(n)
            .and_then(|v| v.checked_add(20))
            .filter(|&v| v <= bytes.len())
            .ok_or_else(|| Malformed {
                rule: Rule::Export,
                detail: "torn snapshot support".into(),
            })?;
        if ty != CHUNK {
            h.update(&bytes[pos..pos + 8]);
            h.update(&bytes[pos + 16..end - 4]);
        }
        if matches!(ty, 2 | 9 | 10 | 18 | 38) {
            clocks += (n + 20) as u64;
        }
        pos = end;
    }
    Ok((h.finish(), clocks))
}

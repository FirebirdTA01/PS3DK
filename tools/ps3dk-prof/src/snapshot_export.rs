//! Raw support projection for a frozen snapshot. The caller selects timing roots;
//! this writer closes structural dependencies and keeps the original writer's final evidence.
use crate::{
    auto_rsx as ar,
    model::*,
    reader::RawRecord,
    snapshot_support::{self as ss, Diagnostic, LaneFrames, Selection},
};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Default, Clone)]
pub struct Keep {
    pub spans: BTreeSet<(u32, u64)>,
    pub ops: BTreeSet<u64>,
    pub frames: BTreeSet<(u32, u64)>,
}

/// A per-snapshot allowance, independent of the source file's length. Exceeding
/// it disables Save for this snapshot, never the snapshot itself.
pub const RECIPE_BYTES: usize = 8 << 20;

/// Immutable, bounded inputs for preparation on the first Save. No Analysis,
/// lane history, raw source copy, or whole operation inventory is retained here.
#[derive(Debug)]
pub struct Frozen {
    selector: Selection,
    keep: Keep,
    seqs: BTreeMap<u32, BTreeSet<u64>>,
    lanes: BTreeMap<u32, ExportLane>,
    ringed: bool,
    last_retained_op: Option<u64>,
    retained_rows: usize,
    ops_total: u64,
    events_total: u64,
    source_ring: Option<ExportRing>,
    fifo_last: u64,
    auto_frames: u64,
    profile_flag: bool,
    flags: u32,
    pub direct_child_bytes: u64,
}
impl Frozen {
    pub fn source_bytes(&self) -> usize {
        self.selector.source_bytes as usize
    }
    pub fn retained_bytes(&self) -> u64 {
        (std::mem::size_of::<Self>()
            + 256
            + (self.keep.ops.len()
                + self.keep.frames.len()
                + self.seqs.values().map(BTreeSet::len).sum::<usize>()
                + self.seqs.len()
                + self.lanes.len()
                + self.selector.lanes.len())
                * 128
            + self.selector.diagnostics.capacity() * 32) as u64
    }
}

#[derive(Debug)]
enum Body {
    Source(std::ops::Range<usize>),
    Owned(Box<[u8]>),
}
#[derive(Debug)]
pub struct Prepared {
    pieces: Vec<(u32, Body, &'static str)>,
    selector: Selection,
    bytes: usize,
    owned: usize,
    profile_flags: u32,
    pub direct_child_bytes: u64,
}
impl Prepared {
    pub fn source_bytes(&self) -> usize {
        self.selector.source_bytes as usize
    }
    pub fn output_bytes(&self) -> usize {
        self.bytes
    }
    pub fn retained_bytes(&self) -> u64 {
        // BTree nodes and allocator slack are accounted conservatively, just as
        // the viewer's snapshot ledger does; this is not a physical RSS claim.
        (std::mem::size_of::<Self>()
            + self.owned
            + self.pieces.capacity() * (std::mem::size_of::<(u32, Body, &'static str)>() + 32)
            + self.selector.lanes.len() * 128
            + self.selector.diagnostics.capacity() * 32
            + 256) as u64
    }
    fn add(&mut self, ty: u32, body: Body, what: &'static str, limit: usize) -> Result<(), String> {
        let n = match &body {
            Body::Source(r) => r.len(),
            Body::Owned(v) => v.len(),
        };
        if self.bytes.checked_add(n + 20).is_none_or(|n| n > limit) {
            return Err(format!(
                "Snapshot support exceeds {limit} bytes: {what} ({n} more bytes)"
            ));
        }
        let owned = match &body {
            Body::Owned(v) => v.len(),
            _ => 0,
        };
        let growth = if self.pieces.len() == self.pieces.capacity() {
            self.pieces.capacity().max(4) * (std::mem::size_of::<(u32, Body, &'static str)>() + 32)
        } else {
            0
        };
        if self
            .retained_bytes()
            .saturating_add((owned + growth + 256) as u64)
            > RECIPE_BYTES as u64
        {
            return Err(format!(
                "Save unavailable: {what} exceeds the {}-byte per-snapshot recipe allowance",
                RECIPE_BYTES
            ));
        }
        self.bytes += n + 20;
        self.owned += owned + usize::from(owned != 0) * 32;
        self.pieces.push((ty, body, what));
        Ok(())
    }
    /// Emit from the same immutable, reader-accepted prefix used to prepare the
    /// recipe. Only the bounded result is parsed; the source is never reparsed.
    pub fn render(
        &self,
        source: &[u8],
        semantic_sha: [u8; 32],
        limit: usize,
    ) -> Result<Vec<u8>, String> {
        if source.len() as u64 != self.selector.source_bytes {
            return Err("Snapshot source prefix changed".into());
        }
        let mut out = source[..16].to_vec();
        out[12..16].copy_from_slice(&self.profile_flags.to_le_bytes());
        let mut seq = 0;
        let mut at = 0;
        for (ty, body, what) in &self.pieces {
            let p = match body {
                Body::Source(r) => &source[r.clone()],
                Body::Owned(v) => v,
            };
            if *ty == ss::CHUNK {
                at = out.len();
            }
            if *ty == CHUNK_EXPORT {
                let mut p = p.to_vec();
                p[4..8].copy_from_slice(&crate::hash::crc32(source).to_le_bytes());
                push(&mut out, &mut seq, *ty, &p, limit.min(ss::MAX_BYTES), what)?;
            } else {
                push(&mut out, &mut seq, *ty, p, limit.min(ss::MAX_BYTES), what)?;
            }
        }
        let (hash, clocks) = ss::support_digest(&out).map_err(|e| e.to_string())?;
        let mut s = self.selector.clone();
        s.source_sha = crate::sha256::digest(source);
        s.semantic_sha = semantic_sha;
        s.support_sha = hash;
        s.clock_bytes = clocks;
        let p = s.encode();
        out[at + 16..at + 16 + p.len()].copy_from_slice(&p);
        let crc = crate::hash::crc32(&out[at..at + 16 + p.len()]);
        out[at + 16 + p.len()..at + 20 + p.len()].copy_from_slice(&crc.to_le_bytes());
        Ok(out)
    }
}
fn u32at(p: &[u8], n: usize) -> u32 {
    u32::from_le_bytes(p[n..n + 4].try_into().unwrap())
}
fn u64at(p: &[u8], n: usize) -> u64 {
    u64::from_le_bytes(p[n..n + 8].try_into().unwrap())
}

#[derive(Clone)]
struct Chunks<'a> {
    bytes: &'a [u8],
    at: usize,
}
impl<'a> Chunks<'a> {
    fn new(bytes: &'a [u8]) -> Result<Self, String> {
        if bytes.len() < 16 {
            return Err("Snapshot source has no preamble".into());
        }
        let mut pos = 16;
        while pos < bytes.len() {
            if bytes.len() - pos < 20 {
                return Err("Save requires the reader-accepted complete-chunk prefix".into());
            }
            let n = u32at(bytes, pos + 4) as usize;
            if n > bytes.len() - pos - 20 {
                return Err("Save requires the reader-accepted complete-chunk prefix".into());
            }
            pos += n + 20;
        }
        Ok(Self { bytes, at: 16 })
    }
}
impl<'a> Iterator for Chunks<'a> {
    type Item = (u32, &'a [u8]);
    fn next(&mut self) -> Option<Self::Item> {
        if self.at == self.bytes.len() {
            return None;
        }
        let p = self.at;
        let n = u32at(self.bytes, p + 4) as usize;
        self.at += n + 20;
        Some((u32at(self.bytes, p), &self.bytes[p + 16..p + 16 + n]))
    }
}

fn recipe_check(entries: usize, what: &str) -> Result<(), String> {
    if entries.saturating_mul(128) > RECIPE_BYTES {
        Err(format!("Save unavailable: {what} exceeds the {RECIPE_BYTES}-byte per-snapshot recipe allowance"))
    } else {
        Ok(())
    }
}

fn span_sequences(
    cap: &Capture,
    keep: &Keep,
    seqs: &mut BTreeMap<u32, BTreeSet<u64>>,
) -> Result<(), String> {
    let mut count = seqs.values().map(BTreeSet::len).sum::<usize>();
    for span in &cap.spans {
        if keep.spans.contains(&(span.incarnation, span.span_id)) {
            recipe_check(count + 3, "PPU direct-child / stack timing dependencies")?;
            let q = seqs.entry(span.incarnation).or_default();
            for n in [Some(span.begin_seq), Some(span.begin_seq + 1), span.end_seq]
                .into_iter()
                .flatten()
            {
                count += usize::from(q.insert(n));
            }
        }
    }
    Ok(())
}

/// Keep loss witnesses which affect retained timing, and the immediate native
/// records around them (a GAP cannot be adjacent to an export GAP). Witness
/// spans need stack ancestors/endpoints, never their unrelated descendants.
fn gap_sequences(
    cap: &Capture,
    cs: Chunks<'_>,
    keep: &mut Keep,
    seqs: &mut BTreeMap<u32, BTreeSet<u64>>,
) -> Result<(), String> {
    if cap.lanes.values().all(|l| l.gap_count == 0) {
        return Ok(());
    }
    let mut work = 0usize;
    loop {
        let before = seqs.values().map(BTreeSet::len).sum::<usize>();
        let mut needed = BTreeSet::new();
        for p in &cap.spans {
            if p.affected && keep.spans.contains(&(p.incarnation, p.span_id)) {
                if p.depth != 0 && p.gaps_before_begin != 0 {
                    needed.insert((p.incarnation, p.gaps_before_begin));
                }
                if p.end_seq.is_none() {
                    needed.insert((p.incarnation, p.gaps_before_begin + 1));
                }
            }
        }
        recipe_check(needed.len() + before, "PPU GAP witnesses")?;
        let mut ordinal = BTreeMap::<u32, u64>::new();
        let mut previous = BTreeMap::new();
        for (ty, p) in cs.clone() {
            if ty != CHUNK_RECORDS {
                continue;
            }
            let id = u32at(p, 0);
            let rs = &p[8..];
            for (i, b) in rs.chunks_exact(RECORD_LEN).enumerate() {
                work += 1;
                if work > 1 << 26 {
                    return Err(
                        "Save unavailable: PPU GAP dependency closure exceeds its work cap".into(),
                    );
                }
                let r = RawRecord::decode(b);
                if r.kind == KIND_GAP && r.flags != GAP_FLAG_EXPORT {
                    let ord = ordinal.entry(id).or_default();
                    *ord += 1;
                    let frame_jump = cap.lanes[&id]
                        .frames
                        .windows(2)
                        .any(|f| f[1].id > f[0].id + 1 && f[0].seq < r.seq && r.seq < f[1].seq);
                    if needed.contains(&(id, *ord)) || frame_jump {
                        let q = seqs.entry(id).or_default();
                        q.insert(r.seq);
                        if let Some(prev) = previous.get(&id) {
                            q.insert(*prev);
                        }
                        // The validated source forbids terminal/adjacent GAPs.
                        q.insert(RawRecord::decode(&rs[(i + 1) * RECORD_LEN..][..RECORD_LEN]).seq);
                    }
                }
                previous.insert(id, r.seq);
            }
        }
        for p in &cap.spans {
            if seqs.get(&p.incarnation).is_some_and(|q| {
                q.contains(&p.begin_seq)
                    || q.contains(&(p.begin_seq + 1))
                    || p.end_seq.is_some_and(|n| q.contains(&n))
            }) {
                recipe_check(keep.spans.len() + 1, "PPU GAP stack witnesses")?;
                keep.spans.insert((p.incarnation, p.span_id));
            }
        }
        for p in cap.spans.iter().rev() {
            if p.parent != 0 && keep.spans.contains(&(p.incarnation, p.span_id)) {
                recipe_check(keep.spans.len() + 1, "PPU GAP stack ancestors")?;
                keep.spans.insert((p.incarnation, p.parent));
            }
        }
        span_sequences(cap, keep, seqs)?;
        if seqs.values().map(BTreeSet::len).sum::<usize>() == before {
            return Ok(());
        }
    }
}
fn push(
    out: &mut Vec<u8>,
    seq: &mut u64,
    ty: u32,
    p: &[u8],
    limit: usize,
    what: &str,
) -> Result<(), String> {
    if out
        .len()
        .checked_add(p.len() + 20)
        .is_none_or(|n| n > limit)
    {
        return Err(format!(
            "Snapshot support exceeds {limit} bytes: {what} ({} more bytes)",
            p.len() + 20
        ));
    }
    let start = out.len();
    out.extend(ty.to_le_bytes());
    out.extend((p.len() as u32).to_le_bytes());
    out.extend(seq.to_le_bytes());
    out.extend(p);
    let crc = crate::hash::crc32(&out[start..]);
    out.extend(crc.to_le_bytes());
    *seq += 1;
    Ok(())
}

/// All input records are validated before selection. Kept records are byte-identical;
/// only export GAPs, outer framing and the two projection records are constructed.
pub fn project(bytes: &[u8], s: Selection, keep: Keep, limit: usize) -> Result<Vec<u8>, String> {
    let cap = crate::reader::read(bytes).map_err(|e| e.to_string())?;
    let semantic = s.semantic_sha;
    let prepared = prepare(&cap, bytes, s, keep, limit)?;
    let out = prepared.render(bytes, semantic, limit)?;
    crate::reader::read(&out).map_err(|e| format!("snapshot support validation: {e}"))?;
    Ok(out)
}

/// Prepare from an existing validated analysis, with no new source parse and no
/// CRC/hash pass. The source must be its immutable complete-chunk prefix.
pub fn freeze(
    cap: &Capture,
    bytes: &[u8],
    mut s: Selection,
    mut keep: Keep,
    limit: usize,
) -> Result<Frozen, String> {
    let limit = limit.min(ss::MAX_BYTES);
    if cap.snapshot.is_some() {
        return Err("Snapshot is already projected; re-save its validated bytes".into());
    }
    // Only contributors need their direct children's durations. An ancestor or
    // an outside child is structural support, not a new recursive timing root.
    let contributors = keep.spans.clone();
    let mut direct_child_bytes = 0;
    for span in &cap.spans {
        let key = (span.incarnation, span.span_id);
        if contributors.contains(&(span.incarnation, span.parent)) && !keep.spans.contains(&key) {
            recipe_check(keep.spans.len() + 1, "PPU direct-child timing support")?;
            keep.spans.insert(key);
            direct_child_bytes += (2 + u64::from(span.end_seq.is_some())) * RECORD_LEN as u64;
        }
    }
    // Parents precede their children in reader order, even with interleaved lanes.
    for span in cap.spans.iter().rev() {
        if span.parent != 0 && keep.spans.contains(&(span.incarnation, span.span_id)) {
            recipe_check(keep.spans.len() + 1, "PPU stack ancestors")?;
            keep.spans.insert((span.incarnation, span.parent));
        }
    }
    let mut seqs: BTreeMap<u32, BTreeSet<u64>> = BTreeMap::new();
    span_sequences(cap, &keep, &mut seqs)?;
    if cap.lanes.values().any(|l| l.gap_count != 0) {
        gap_sequences(cap, Chunks::new(bytes)?, &mut keep, &mut seqs)?;
    }
    // Original frame marks are attribution/map inputs, not zone bars. Keeping them
    // preserves cross-lane tokens and the EXT 'other frame' placement test exactly.
    recipe_check(
        cap.lanes.len().saturating_mul(8),
        "source lane manifests and diagnostics",
    )?;
    s.lanes = cap
        .lanes
        .iter()
        .map(|(&id, l)| {
            (
                id,
                LaneFrames {
                    first: l.frames.first().map_or(0, |f| f.id),
                    last: l.frames.last().map_or(0, |f| f.id),
                    count: l.frames.len() as u64,
                },
            )
        })
        .collect();
    s.diagnostics.clear();
    for (&id, l) in &cap.lanes {
        let only = !l.frames.is_empty()
            && l.spans.is_empty()
            && l.written == l.frames.len() as u64
            && l.gap_count == 0
            && !l.clock_anomaly;
        for (kind, value) in [
            (1, u64::from(only)),
            (2, u64::from(l.clock_anomaly)),
            (3, l.gap_count),
        ] {
            s.diagnostics.push(Diagnostic {
                kind,
                source_kind: 1,
                source: id,
                key: 0,
                value,
                auxiliary: 0,
            });
        }
    }
    if let Some(a) = &cap.auto {
        s.diagnostics.push(Diagnostic {
            kind: 4,
            source_kind: 4,
            source: 0,
            key: 0,
            value: a.ops_dropped,
            auxiliary: 0,
        });
    }
    if cap.checkpoints.is_some() {
        s.diagnostics.push(Diagnostic {
            kind: 5,
            source_kind: 0,
            source: 0,
            key: 0,
            value: 0, // filled from frozen source chunks by the Save worker
            auxiliary: 0,
        });
    }
    s.diagnostics.sort();
    s.diagnostics.push(Diagnostic {
        kind: 6,
        source_kind: 0,
        source: 0,
        key: 0,
        value: direct_child_bytes,
        auxiliary: 0,
    });
    let lanes: BTreeMap<u32, ExportLane> = cap
        .lanes
        .iter()
        .map(|(&id, l)| {
            let total = cap
                .export
                .as_ref()
                .and_then(|e| e.lanes.get(&id))
                .map_or(l.written, |e| e.kept + e.dropped_by_export);
            (
                id,
                ExportLane {
                    incarnation: id,
                    first_seq: 0,
                    last_seq: 0,
                    kept: 0,
                    dropped_by_export: total,
                },
            )
        })
        .collect();
    let inv = cap.auto.as_ref();
    let ringed = inv
        .and_then(|a| a.config.as_ref())
        .and_then(ar::Config::attached)
        .is_some_and(|a| a.ringed());
    let ops_total = cap.export.as_ref().map_or_else(
        || inv.map_or(0, |a| a.ops_parsed as u64),
        |e| e.ops_kept + e.ops_dropped,
    );
    let events_total = cap.export.as_ref().map_or_else(
        || cap.fifo.as_ref().map_or(0, |f| f.parsed_events),
        |e| e.events_kept + e.events_dropped,
    );
    keep.spans.clear(); // raw sequence selectors already close this dependency
    let out = Frozen {
        selector: s,
        keep,
        seqs,
        lanes,
        ringed,
        last_retained_op: inv.and_then(|a| a.ops.last()).map(|o| o.id),
        retained_rows: inv.map_or(0, |a| a.frames.len()),
        ops_total,
        events_total,
        source_ring: cap.export.as_ref().and_then(|e| e.ring),
        fifo_last: cap
            .fifo
            .as_ref()
            .and_then(|f| f.last_status.as_ref())
            .map_or(0, |s| s.last_attempt_seq),
        auto_frames: inv
            .and_then(|a| a.last_status.as_ref())
            .map_or(0, |s| s.counters[5]),
        profile_flag: cap.profile_flag,
        flags: u32at(bytes, 12) & !crate::checkpoints::FLAG_FRAME_CHECKPOINTS,
        direct_child_bytes,
    };
    if out.retained_bytes() > RECIPE_BYTES as u64 {
        return Err(
            "Save unavailable: frozen dependency inputs exceed the per-snapshot allowance".into(),
        );
    }
    let _ = limit; // output cap is checked by the lazy projection on Save
    Ok(out)
}

pub fn prepare(
    cap: &Capture,
    bytes: &[u8],
    s: Selection,
    keep: Keep,
    limit: usize,
) -> Result<Prepared, String> {
    freeze(cap, bytes, s, keep, limit)?.prepare(bytes, limit)
}

impl Frozen {
    pub fn prepare(&self, bytes: &[u8], limit: usize) -> Result<Prepared, String> {
        if bytes.len() != self.source_bytes() {
            return Err("Snapshot source prefix changed".into());
        }
        let limit = limit.min(ss::MAX_BYTES);
        let cs = Chunks::new(bytes)?;
        let mut s = self.selector.clone();
        let mut keep = self.keep.clone();
        // Reconcile execution/build dependencies lazily over the immutable raw
        // records. Respect the original reader retention frontier: a record it
        // did not retain must never become new snapshot timing.
        let mut work = 0usize;
        loop {
            let before = keep.frames.len();
            for (ty, p) in cs.clone() {
                if ty != ar::CHUNK_AUTO_OPS {
                    continue;
                }
                let width = u32at(p, 12) as usize;
                for o in p[16..].chunks_exact(width) {
                    work += 1;
                    if work > 1 << 26 {
                        return Err(
                            "Save unavailable: AUTO build/execution closure exceeds its work cap"
                                .into(),
                        );
                    }
                    let id = u64at(o, 0);
                    if self.last_retained_op.is_none_or(|last| id > last) {
                        continue;
                    }
                    let token = (u16::from_le_bytes([o[14], o[15]]) & ar::OP_TOKEN != 0)
                        .then(|| (u32at(o, 8), u64at(o, 16)));
                    let build = (width >= ar::OP_BYTES_BUILD as usize
                        && u16::from_le_bytes([o[140], o[141]]) & ar::OP_BUILD_KNOWN != 0)
                        .then(|| (u32at(o, 136), u64at(o, 128)));
                    if keep.ops.contains(&id)
                        || token.is_some_and(|f| keep.frames.contains(&f))
                        || build.is_some_and(|f| keep.frames.contains(&f))
                    {
                        recipe_check(
                            keep.ops.len() + keep.frames.len() + 3,
                            "AUTO execution/build timing support",
                        )?;
                        keep.ops.insert(id);
                        if let Some(f) = token {
                            keep.frames.insert(f);
                        }
                        if let Some(f) = build {
                            keep.frames.insert(f);
                        }
                    }
                }
            }
            if before == keep.frames.len() {
                break;
            }
        }
        if let Some(d) = s.diagnostics.iter_mut().find(|d| d.kind == 5) {
            d.value = cs.clone().filter(|(ty, _)| *ty == 26).count() as u64;
        }
        s.auto_rows = cs
            .clone()
            .filter(|(ty, _)| *ty == ar::CHUNK_AUTO_COVERAGE)
            .take(self.retained_rows)
            .filter(|(_, p)| keep.frames.contains(&(u32at(p, 8), u64at(p, 16))))
            .count() as u64;
        let ops_kept = cs
            .clone()
            .filter(|(ty, _)| *ty == ar::CHUNK_AUTO_OPS)
            .map(|(_, p)| {
                p[16..]
                    .chunks_exact(u32at(p, 12) as usize)
                    .filter(|o| keep.ops.contains(&u64at(o, 0)))
                    .count() as u64
            })
            .sum::<u64>();
        let seqs = &self.seqs;
        let mut lanes = self.lanes.clone();
        let (ringed, ops_total, events_total) = (self.ringed, self.ops_total, self.events_total);
        let direct_child_bytes = self.direct_child_bytes;
        let mut records = BTreeSet::new();
        for (ty, p) in cs.clone() {
            if ty == CHUNK_RECORDS {
                let id = u32at(p, 0);
                for b in p[8..].chunks_exact(RECORD_LEN) {
                    let r = RawRecord::decode(b);
                    let take = matches!(r.kind, KIND_FRAME | KIND_REGERR)
                        || seqs.get(&id).is_some_and(|v| v.contains(&r.seq));
                    if take {
                        recipe_check(records.len() + 1, "PPU timing / frame / GAP records")?;
                        records.insert((id, r.seq));
                        if r.kind != KIND_GAP {
                            let e = lanes.get_mut(&id).unwrap();
                            if e.kept == 0 {
                                e.first_seq = r.seq;
                            }
                            e.last_seq = r.seq;
                            e.kept += 1;
                            e.dropped_by_export -= 1;
                        }
                    }
                }
            }
        }
        let mut ring = ExportRing {
            first_generation: 0,
            last_generation: 0,
            generations: 0,
            records_kept: 0,
            records_dropped: 0,
            source_last_generation: 0,
        };
        for (ty, p) in cs.clone() {
            if ty == ar::CHUNK_AUTO_OPS && u32at(p, 12) == ar::OP_BYTES_RING {
                for o in p[16..].chunks_exact(ar::OP_BYTES_RING as usize) {
                    if u16::from_le_bytes(o[14..16].try_into().unwrap()) & ar::OP_RING == 0 {
                        continue;
                    }
                    let generation = u32at(o, 160);
                    ring.source_last_generation = ring.source_last_generation.max(generation);
                    if keep.ops.contains(&u64at(o, 0)) {
                        if ring.records_kept == 0 {
                            ring.first_generation = generation;
                        }
                        if ring.records_kept == 0 || ring.last_generation != generation {
                            ring.generations += 1;
                        }
                        ring.last_generation = generation;
                        ring.records_kept += 1;
                    } else {
                        ring.records_dropped += 1;
                    }
                }
            }
        }
        if let Some(old) = self.source_ring {
            ring.source_last_generation = old.source_last_generation;
            ring.records_dropped += old.records_dropped;
        }
        let mut exp = Vec::new();
        exp.extend(EXPORT_SCHEMA.to_le_bytes());
        exp.extend(
            (EXPORT_FLAG_TO_END | ss::EXPORT_FLAG | if ringed { EXPORT_FLAG_RING } else { 0 })
                .to_le_bytes(),
        );
        exp.extend(0u32.to_le_bytes()); // source CRC is computed by the save worker
        exp.extend((bytes.len() as u64).to_le_bytes());
        for n in [
            s.frame,
            s.frame,
            0,
            lanes.values().map(|l| l.kept).sum(),
            lanes.values().map(|l| l.dropped_by_export).sum(),
            ops_kept,
            ops_total - ops_kept,
            0,
            events_total,
            self.fifo_last,
            self.auto_frames,
        ] {
            exp.extend(n.to_le_bytes());
        }
        exp.extend((lanes.len() as u32).to_le_bytes());
        exp.extend(0u32.to_le_bytes());
        for e in lanes.values() {
            exp.extend(e.incarnation.to_le_bytes());
            exp.extend(0u32.to_le_bytes());
            for n in [e.first_seq, e.last_seq, e.kept, e.dropped_by_export] {
                exp.extend(n.to_le_bytes());
            }
        }
        if ringed {
            exp.extend(ring.first_generation.to_le_bytes());
            exp.extend(ring.last_generation.to_le_bytes());
            for n in [ring.generations, ring.records_kept, ring.records_dropped] {
                exp.extend(n.to_le_bytes());
            }
            exp.extend(ring.source_last_generation.to_le_bytes());
            exp.extend(0u32.to_le_bytes());
        }
        // Snapshot resolution does not consume source proofs. Their original outer
        // chunk references cannot be retained after projection; carry omission/count
        // as provenance, without presenting a new completion proof.
        let flags = self.flags;
        let mut out = Prepared {
            pieces: Vec::new(),
            selector: s.clone(),
            bytes: 16,
            owned: 0,
            profile_flags: flags,
            direct_child_bytes,
        };
        let mut prev = BTreeMap::new();
        let mut coverage_ordinal = 0;
        for (ty, p) in cs {
            let mut q = Vec::new();
            let body = match ty {
                CHUNK_EXPORT | 19..=21 | 25 | 26 => continue,
                CHUNK_RECORDS => {
                    let id = u32at(p, 0);
                    q.extend(&p[..8]);
                    for b in p[8..].chunks_exact(RECORD_LEN) {
                        let r = RawRecord::decode(b);
                        if !records.contains(&(id, r.seq)) {
                            continue;
                        }
                        if let Some(&last) = prev.get(&id) {
                            if r.seq > last + 1 {
                                q.extend(crate::export::encode_gap(last + 1, r.seq - 1, true));
                            }
                        }
                        q.extend(b);
                        prev.insert(id, if r.kind == KIND_GAP { r.c } else { r.seq });
                    }
                    if q.len() == 8 {
                        continue;
                    }
                    let n = ((q.len() - 8) / 32) as u32;
                    q[4..8].copy_from_slice(&n.to_le_bytes());
                    &q[..]
                }
                ar::CHUNK_AUTO_OPS => {
                    let width = u32at(p, 12) as usize;
                    let ops = &p[16..];
                    // The reader validated strictly increasing IDs. Most chunks
                    // in a long session cannot contribute to this one snapshot.
                    if ops.is_empty()
                        || keep
                            .ops
                            .range(u64at(ops, 0)..=u64at(ops, ops.len() - width))
                            .next()
                            .is_none()
                    {
                        continue;
                    }
                    q.extend(&p[..16]);
                    for o in ops.chunks_exact(width) {
                        if keep.ops.contains(&u64at(o, 0)) {
                            q.extend(o);
                        }
                    }
                    if q.len() == 16 {
                        continue;
                    }
                    let n = ((q.len() - 16) / width) as u32;
                    q[8..12].copy_from_slice(&n.to_le_bytes());
                    &q[..]
                }
                ar::CHUNK_AUTO_COVERAGE => {
                    coverage_ordinal += 1;
                    if coverage_ordinal > self.retained_rows {
                        continue;
                    }
                    if !keep.frames.contains(&(u32at(p, 8), u64at(p, 16))) {
                        continue;
                    }
                    p
                }
                crate::fifo::CHUNK_FIFO_EVENTS => continue,
                crate::fifo::CHUNK_FIFO_STATUS => {
                    if u16::from_le_bytes(p[2..4].try_into().unwrap()) & crate::fifo::ST_FINAL == 0
                    {
                        continue;
                    }
                    p
                }
                43 => {
                    if (u32at(p, 16), u64at(p, 24)) != (s.owner, s.frame) {
                        continue;
                    }
                    p
                }
                _ => p,
            };
            let what = match ty {
                5 if direct_child_bytes != 0 => {
                    "PPU direct-child timing support / stack / frame witnesses"
                }
                5 => "PPU timing/stack/frame witnesses",
                8..=18 => "EXT epoch/map inputs",
                38 => "AUTO clock anchors",
                35 | 36 => "AUTO execution/build closure",
                _ => "source metadata/auxiliary structural closure",
            };
            let body = if q.is_empty() {
                let start = body.as_ptr() as usize - bytes.as_ptr() as usize;
                Body::Source(start..start + body.len())
            } else {
                Body::Owned(q.into_boxed_slice())
            };
            out.add(ty, body, what, limit)?;
            if ty == if self.profile_flag { 17 } else { 2 } {
                out.add(
                    CHUNK_EXPORT,
                    Body::Owned(exp.clone().into_boxed_slice()),
                    "EXPORT accounting",
                    limit,
                )?;
                out.add(
                    ss::CHUNK,
                    Body::Owned(s.encode().into_boxed_slice()),
                    "snapshot selector/diagnostics",
                    limit,
                )?;
            }
        }
        Ok(out)
    }
}

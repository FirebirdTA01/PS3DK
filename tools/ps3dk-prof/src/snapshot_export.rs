//! Raw support projection for a frozen snapshot. The caller selects timing roots;
//! this writer closes structural dependencies and keeps the original writer's final evidence.
use crate::{
    auto_rsx as ar,
    model::*,
    reader::RawRecord,
    snapshot_support::{self as ss, Diagnostic, LaneFrames, Selection},
};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Default)]
pub struct Keep {
    pub spans: BTreeSet<(u32, u64)>,
    pub ops: BTreeSet<u64>,
    pub frames: BTreeSet<(u32, u64)>,
}
fn u32at(p: &[u8], n: usize) -> u32 {
    u32::from_le_bytes(p[n..n + 4].try_into().unwrap())
}
fn u64at(p: &[u8], n: usize) -> u64 {
    u64::from_le_bytes(p[n..n + 8].try_into().unwrap())
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
pub fn project(
    bytes: &[u8],
    mut s: Selection,
    mut keep: Keep,
    limit: usize,
) -> Result<Vec<u8>, String> {
    let limit = limit.min(ss::MAX_BYTES);
    let cap = crate::reader::read(bytes).map_err(|e| e.to_string())?;
    if cap.snapshot.is_some() {
        return Err("Snapshot is already projected; re-save its validated bytes".into());
    }
    // Parent and child closure preserves stack depth and exact exclusive time.
    // Computing connected roots in source order avoids a scan per nesting level.
    let mut roots = BTreeMap::new();
    for span in &cap.spans {
        let key = (span.incarnation, span.span_id);
        let root = roots
            .get(&(span.incarnation, span.parent))
            .copied()
            .unwrap_or(key);
        roots.insert(key, root);
    }
    let wanted: BTreeSet<_> = keep
        .spans
        .iter()
        .filter_map(|k| roots.get(k).copied())
        .collect();
    for (key, root) in &roots {
        if wanted.contains(root) {
            keep.spans.insert(*key);
        }
    }
    let mut seqs: BTreeMap<u32, BTreeSet<u64>> = BTreeMap::new();
    for span in &cap.spans {
        if keep.spans.contains(&(span.incarnation, span.span_id)) {
            let q = seqs.entry(span.incarnation).or_default();
            q.insert(span.begin_seq);
            q.insert(span.begin_seq + 1);
            if let Some(end) = span.end_seq {
                q.insert(end);
            }
        }
    }
    // Retain a complete frame group for every AUTO dependency: ring generations
    // must not be cut inside a frame, and build/execution populations reconcile together.
    if let Some(a) = &cap.auto {
        let mut by_frame: BTreeMap<(u32, u64), Vec<usize>> = BTreeMap::new();
        for (i, o) in a.ops.iter().enumerate() {
            for f in [o.token(), o.build].into_iter().flatten() {
                by_frame.entry(f).or_default().push(i);
                if keep.ops.contains(&o.id) {
                    keep.frames.insert(f);
                }
            }
        }
        let mut queue: std::collections::VecDeque<_> = keep.frames.iter().copied().collect();
        while let Some(f) = queue.pop_front() {
            if let Some(indices) = by_frame.remove(&f) {
                for i in indices {
                    let o = &a.ops[i];
                    keep.ops.insert(o.id);
                    for next in [o.token(), o.build].into_iter().flatten() {
                        if keep.frames.insert(next) {
                            queue.push_back(next);
                        }
                    }
                }
            }
        }
    }
    let mut cs: Vec<(u32, &[u8])> = Vec::new();
    let mut pos = 16;
    while pos < bytes.len() {
        if bytes.len() - pos < 20 {
            return Err("Save requires the reader-accepted complete-chunk prefix".into());
        }
        let n = u32at(bytes, pos + 4) as usize;
        if n > bytes.len() - pos - 20 {
            return Err("Save requires the reader-accepted complete-chunk prefix".into());
        }
        cs.push((u32at(bytes, pos), &bytes[pos + 16..pos + 16 + n]));
        pos += 20 + n;
    }
    // Original frame marks are attribution/map inputs, not zone bars. Keeping them
    // preserves cross-lane tokens and the EXT 'other frame' placement test exactly.
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
            value: cs.iter().filter(|(ty, _)| *ty == 26).count() as u64,
            auxiliary: 0,
        });
    }
    s.diagnostics.sort();
    let mut lanes: BTreeMap<u32, ExportLane> = cap
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
    let mut records = BTreeSet::new();
    for (ty, p) in &cs {
        if *ty == CHUNK_RECORDS {
            let id = u32at(p, 0);
            let l = &cap.lanes[&id];
            for b in p[8..].chunks_exact(RECORD_LEN) {
                let r = RawRecord::decode(b);
                // A lossy lane's raw stack witnesses are indivisible. Its entire raw
                // lane is named as loss-context support and remains under the same cap.
                let take = l.gap_count > 0
                    || matches!(r.kind, KIND_FRAME | KIND_REGERR)
                    || seqs.get(&id).is_some_and(|v| v.contains(&r.seq));
                if take {
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
    let inv = cap.auto.as_ref();
    let ringed = inv
        .and_then(|a| a.config.as_ref())
        .and_then(ar::Config::attached)
        .is_some_and(|a| a.ringed());
    let ops_kept = inv.map_or(0, |a| {
        a.ops.iter().filter(|o| keep.ops.contains(&o.id)).count() as u64
    });
    let ops_total = cap.export.as_ref().map_or_else(
        || inv.map_or(0, |a| a.ops_parsed as u64),
        |e| e.ops_kept + e.ops_dropped,
    );
    s.auto_rows = inv.map_or(0, |a| {
        a.frames
            .iter()
            .filter(|r| keep.frames.contains(&(r.owner, r.frame)))
            .count() as u64
    });
    let mut ring = ExportRing {
        first_generation: 0,
        last_generation: 0,
        generations: 0,
        records_kept: 0,
        records_dropped: 0,
        source_last_generation: 0,
    };
    for (ty, p) in &cs {
        if *ty == ar::CHUNK_AUTO_OPS && u32at(p, 12) == ar::OP_BYTES_RING {
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
    if let Some(old) = cap.export.as_ref().and_then(|e| e.ring) {
        ring.source_last_generation = old.source_last_generation;
        ring.records_dropped += old.records_dropped;
    }
    let events_total = cap.export.as_ref().map_or_else(
        || cap.fifo.as_ref().map_or(0, |f| f.parsed_events),
        |e| e.events_kept + e.events_dropped,
    );
    let mut exp = Vec::new();
    exp.extend(EXPORT_SCHEMA.to_le_bytes());
    exp.extend(
        (EXPORT_FLAG_TO_END | ss::EXPORT_FLAG | if ringed { EXPORT_FLAG_RING } else { 0 })
            .to_le_bytes(),
    );
    exp.extend(crate::hash::crc32(bytes).to_le_bytes());
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
        cap.fifo
            .as_ref()
            .and_then(|f| f.last_status.as_ref())
            .map_or(0, |s| s.last_attempt_seq),
        inv.and_then(|a| a.last_status.as_ref())
            .map_or(0, |s| s.counters[5]),
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
    let mut out = bytes[..16].to_vec();
    // Snapshot resolution does not consume source proofs. Their original outer
    // chunk references cannot be retained after projection; carry omission/count
    // as provenance, without presenting a new completion proof.
    let flags = u32at(&out, 12) & !crate::checkpoints::FLAG_FRAME_CHECKPOINTS;
    out[12..16].copy_from_slice(&flags.to_le_bytes());
    let mut seq = 0;
    let mut selector_pos = 0;
    let mut prev = BTreeMap::new();
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
                q.extend(&p[..16]);
                let width = u32at(p, 12) as usize;
                for o in p[16..].chunks_exact(width) {
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
                if !keep.frames.contains(&(u32at(p, 8), u64at(p, 16))) {
                    continue;
                }
                p
            }
            crate::fifo::CHUNK_FIFO_EVENTS => continue,
            crate::fifo::CHUNK_FIFO_STATUS => {
                if u16::from_le_bytes(p[2..4].try_into().unwrap()) & crate::fifo::ST_FINAL == 0 {
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
            5 => "PPU timing/stack/frame witnesses",
            8..=18 => "EXT epoch/map inputs",
            38 => "AUTO clock anchors",
            35 | 36 => "AUTO execution/build closure",
            _ => "source metadata/auxiliary structural closure",
        };
        push(&mut out, &mut seq, ty, body, limit, what)?;
        if ty == if cap.profile_flag { 17 } else { 2 } {
            push(
                &mut out,
                &mut seq,
                CHUNK_EXPORT,
                &exp,
                limit,
                "EXPORT accounting",
            )?;
            selector_pos = out.len();
            push(
                &mut out,
                &mut seq,
                ss::CHUNK,
                &s.encode(),
                limit,
                "snapshot selector/diagnostics",
            )?;
        }
    }
    let (hash, clocks) = ss::support_digest(&out).map_err(|e| e.to_string())?;
    s.support_sha = hash;
    s.clock_bytes = clocks;
    let p = s.encode();
    out[selector_pos + 16..selector_pos + 16 + p.len()].copy_from_slice(&p);
    let crc = crate::hash::crc32(&out[selector_pos..selector_pos + 16 + p.len()]);
    out[selector_pos + 16 + p.len()..selector_pos + 20 + p.len()]
        .copy_from_slice(&crc.to_le_bytes());
    crate::reader::read(&out).map_err(|e| format!("snapshot support validation: {e}"))?;
    Ok(out)
}

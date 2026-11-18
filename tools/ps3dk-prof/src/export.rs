//! Standalone capture frame export: one frame or a frame range saved as an independent .prof capture.

use std::collections::{BTreeMap, BTreeSet};
use std::ops::RangeInclusive;
use std::path::Path;

use crate::auto_rsx::{
    CHUNK_AUTO_COVERAGE, CHUNK_AUTO_OPS, CHUNK_AUTO_STATUS,
    OP_BYTES, OP_BYTES_BUILD, OP_BUILD_KNOWN, OP_RING, OP_TOKEN,
};
use crate::fifo::{CHUNK_FIFO_EVENTS, CHUNK_FIFO_STATUS, EV_TOKEN_PRESENT};
use crate::hash::crc32;
use crate::model::*;
use crate::reader::RawRecord;

#[derive(Debug)]
pub enum ExportError {
    Io(std::io::Error),
    Malformed(String),
    Refused(&'static str),
    FrameNotFound(u64),
    Usage(String),
}

impl std::fmt::Display for ExportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ExportError::Io(e) => write!(f, "{e}"),
            ExportError::Malformed(why) => write!(f, "MALFORMED: {why}"),
            ExportError::Refused(why) => write!(f, "export refused: {why}"),
            ExportError::FrameNotFound(frame) => {
                write!(f, "export: frame {frame} is not in the capture")
            }
            ExportError::Usage(msg) => write!(f, "{msg}"),
        }
    }
}

impl std::error::Error for ExportError {}

struct RawChunk<'a> {
    ty: u32,
    payload: &'a [u8],
}

fn encode_record(r: &RawRecord) -> [u8; 32] {
    let mut buf = [0u8; 32];
    buf[0] = r.kind;
    buf[1] = r.depth;
    buf[2..4].copy_from_slice(&r.flags.to_le_bytes());
    buf[4..8].copy_from_slice(&r.a.to_le_bytes());
    buf[8..16].copy_from_slice(&r.seq.to_le_bytes());
    buf[16..24].copy_from_slice(&r.b.to_le_bytes());
    buf[24..32].copy_from_slice(&r.c.to_le_bytes());
    buf
}

fn encode_gap(start_seq: u64, end_seq: u64, is_export: bool) -> [u8; 32] {
    let mut buf = [0u8; 32];
    buf[0] = KIND_GAP;
    if is_export {
        buf[2..4].copy_from_slice(&GAP_FLAG_EXPORT.to_le_bytes());
    }
    buf[8..16].copy_from_slice(&start_seq.to_le_bytes());
    buf[16..24].copy_from_slice(&start_seq.to_le_bytes());
    buf[24..32].copy_from_slice(&end_seq.to_le_bytes());
    buf
}

/// Export one frame or a range of frames from capture bytes into standalone capture bytes.
pub fn export_bytes(bytes: &[u8], range: RangeInclusive<u64>) -> Result<Vec<u8>, ExportError> {
    if bytes.len() < 16 {
        return Err(ExportError::Malformed("capture header shorter than 16 bytes".into()));
    }
    if &bytes[0..8] != b"PS3DKPRF" {
        return Err(ExportError::Malformed("bad magic".into()));
    }
    let major = u16::from_le_bytes(bytes[8..10].try_into().unwrap());
    if major != 1 {
        return Err(ExportError::Malformed("unsupported major version".into()));
    }

    // 1. Structural parse of the source capture using the reader.
    let cap = crate::reader::read(bytes).map_err(|_| ExportError::Malformed("source capture malformed".into()))?;

    if cap.export.is_some() {
        return Err(ExportError::Refused("re-exporting an already-exported capture is not supported"));
    }

    // 2. Collect all present frames and verify the requested range.
    let mut present_frames = BTreeSet::new();
    if let Some(auto) = &cap.auto {
        for cov in &auto.frames {
            present_frames.insert(cov.frame);
        }
    }
    for lane in cap.lanes.values() {
        for fr in &lane.frames {
            present_frames.insert(fr.id);
        }
    }

    for f in range.clone() {
        if !present_frames.contains(&f) {
            return Err(ExportError::FrameNotFound(f));
        }
    }

    // 3. Negotiated op width from AUTO_CONFIG if present.
    let attached = cap.auto.as_ref().and_then(|a| a.config.as_ref()).and_then(|c| c.attached());
    let op_width = attached.map_or(OP_BYTES, |att| att.op_width()) as usize;
    // a report-ring source (2047/11): the export accounts for the RING generations it keeps
    let ringed = attached.is_some_and(|att| att.ringed());

    // 4. Determine frame owner lane and time bounds.
    let (&owner_inc, owner_lane) = cap
        .lanes
        .iter()
        .find(|(_, l)| l.frames.iter().any(|fr| fr.id == *range.start()))
        .ok_or(ExportError::FrameNotFound(*range.start()))?;
    let mark_a = owner_lane
        .frames
        .iter()
        .find(|fr| fr.id == *range.start())
        .unwrap();
    let tb_start = mark_a.tb;
    let owner_start_seq = mark_a.seq;

    let mark_b1 = owner_lane
        .frames
        .iter()
        .find(|fr| fr.id == *range.end() + 1);
    let to_end_of_run = mark_b1.is_none();
    let tb_end = mark_b1.map(|fr| fr.tb);
    let owner_end_seq = mark_b1.map(|fr| fr.seq);

    // 5. Detect clipped zones:
    // Any zone crossing window edges:
    // - BEGIN before mark A whose END is inside (or open)
    // - BEGIN inside whose END is after mark B+1 (or open, when B+1 exists)
    // Drop both halves of any zone not fully inside the window (nesting included).
    // For the owner lane, boundaries are exact sequence numbers matching record filtering;
    // for worker lanes without frame marks, timestamps are used.
    let mut clipped_span_ids: BTreeSet<(u32, u64)> = BTreeSet::new();
    for span in &cap.spans {
        let (crosses_left, crosses_right) = if span.incarnation == owner_inc {
            let left = span.begin_seq < owner_start_seq
                && span.end_seq.map_or(true, |eseq| eseq >= owner_start_seq);
            let right = if let Some(end_seq) = owner_end_seq {
                span.begin_seq <= end_seq && span.end_seq.map_or(true, |eseq| eseq > end_seq)
            } else {
                false
            };
            (left, right)
        } else {
            let left = span.begin_tb < tb_start
                && span.end_tb.map_or(true, |etb| etb >= tb_start);
            let right = if let Some(etb) = tb_end {
                span.begin_tb <= etb && span.end_tb.map_or(true, |end_tb| end_tb > etb)
            } else {
                false
            };
            (left, right)
        };
        if crosses_left || crosses_right {
            clipped_span_ids.insert((span.incarnation, span.span_id));
        }
    }

    // Propagate to nested descendants
    let mut changed = true;
    while changed {
        changed = false;
        for span in &cap.spans {
            if !clipped_span_ids.contains(&(span.incarnation, span.span_id)) {
                if clipped_span_ids.contains(&(span.incarnation, span.parent)) {
                    clipped_span_ids.insert((span.incarnation, span.span_id));
                    changed = true;
                }
            }
        }
    }
    let clipped_zones = clipped_span_ids.len() as u64;

    // Collect sequence numbers of records belonging to clipped zones
    let mut clipped_record_seqs: BTreeMap<u32, BTreeSet<u64>> = BTreeMap::new();
    for span in &cap.spans {
        if clipped_span_ids.contains(&(span.incarnation, span.span_id)) {
            let set = clipped_record_seqs.entry(span.incarnation).or_default();
            set.insert(span.begin_seq);
            set.insert(span.begin_seq + 1);
            if let Some(end_seq) = span.end_seq {
                set.insert(end_seq);
            }
        }
    }

    // 6. Linear walk to parse raw chunks from bytes.
    let mut raw_chunks = Vec::new();
    let mut pos = 16;
    while pos < bytes.len() {
        if bytes.len() - pos < 20 {
            return Err(ExportError::Malformed("truncated chunk header".into()));
        }
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        if ty == CHUNK_EXPORT {
            return Err(ExportError::Refused("re-exporting an already-exported capture is not supported"));
        }
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if bytes.len() - pos < 16 + len + 4 {
            return Err(ExportError::Malformed("truncated chunk payload".into()));
        }
        let payload = &bytes[pos + 16..pos + 16 + len];
        let file_crc =
            u32::from_le_bytes(bytes[pos + 16 + len..pos + 20 + len].try_into().unwrap());
        let computed_crc = crc32(&bytes[pos..pos + 16 + len]);
        if file_crc != computed_crc {
            return Err(ExportError::Malformed("chunk CRC mismatch".into()));
        }
        raw_chunks.push(RawChunk { ty, payload });
        pos += 20 + len;
    }

    // Collect all raw records per lane from CHUNK_RECORDS
    let mut raw_lane_records: BTreeMap<u32, Vec<RawRecord>> = BTreeMap::new();
    for chunk in &raw_chunks {
        if chunk.ty == CHUNK_RECORDS && chunk.payload.len() >= 8 {
            let inc = u32::from_le_bytes(chunk.payload[0..4].try_into().unwrap());
            let n = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
            for i in 0..n {
                let off = 8 + i * RECORD_LEN;
                if off + RECORD_LEN <= chunk.payload.len() {
                    let rec = RawRecord::decode(&chunk.payload[off..off + RECORD_LEN]);
                    raw_lane_records.entry(inc).or_default().push(rec);
                }
            }
        }
    }

    // Filter records per lane and build per-lane export table
    let mut export_lanes: BTreeMap<u32, ExportLane> = BTreeMap::new();
    let mut kept_record_keys: BTreeSet<(u32, u64)> = BTreeSet::new();
    let mut records_kept_total = 0u64;
    let mut records_dropped_total = 0u64;

    for (&inc, lane) in &cap.lanes {
        let is_owner = lane.is_frame_owner();
        let all_recs = raw_lane_records.get(&inc).map(|v| v.as_slice()).unwrap_or(&[]);

        let mut worker_window_seq_range: Option<(u64, u64)> = None;
        if !is_owner {
            let mut w_last_tb = None;
            let mut min_seq = None;
            let mut max_seq = None;
            for rec in all_recs {
                let tb = match rec.kind {
                    KIND_BEGIN => {
                        w_last_tb = Some(rec.b);
                        Some(rec.b)
                    }
                    KIND_BEGIN_PARENT => w_last_tb,
                    KIND_END | KIND_REGERR => Some(rec.b),
                    _ => None,
                };
                if let Some(t) = tb {
                    let in_win = t >= tb_start && (if let Some(end_tb) = tb_end { t <= end_tb } else { true });
                    if in_win {
                        if min_seq.is_none() {
                            min_seq = Some(rec.seq);
                        }
                        max_seq = Some(rec.seq);
                    }
                }
            }
            if let (Some(s), Some(e)) = (min_seq, max_seq) {
                worker_window_seq_range = Some((s, e));
            }
        }

        let mut kept_records = Vec::new();
        let mut last_begin_tb: Option<u64> = None;

        for rec in all_recs {
            let rec_tb = match rec.kind {
                KIND_BEGIN => {
                    last_begin_tb = Some(rec.b);
                    Some(rec.b)
                }
                KIND_BEGIN_PARENT => last_begin_tb,
                KIND_END | KIND_FRAME | KIND_REGERR => Some(rec.b),
                _ => None,
            };

            let in_window = if rec.kind == KIND_GAP {
                if is_owner {
                    rec.seq >= owner_start_seq
                        && (if let Some(end_seq) = owner_end_seq {
                            rec.c <= end_seq
                        } else {
                            true
                        })
                } else if let Some((ws, we)) = worker_window_seq_range {
                    rec.seq >= ws && rec.c <= we
                } else {
                    false
                }
            } else if is_owner {
                rec.seq >= owner_start_seq
                    && (if let Some(end_seq) = owner_end_seq {
                        rec.seq <= end_seq
                    } else {
                        true
                    })
            } else if let Some(tb) = rec_tb {
                tb >= tb_start
                    && (if let Some(end_tb) = tb_end {
                        tb <= end_tb
                    } else {
                        true
                    })
            } else {
                false
            };

            let is_clipped = clipped_record_seqs
                .get(&inc)
                .map_or(false, |set| set.contains(&rec.seq));

            if in_window && !is_clipped {
                kept_records.push(*rec);
                kept_record_keys.insert((inc, rec.seq));
            }
        }

        // If no non-GAP records survive, drop any writer GAPs as well so the lane is fully dropped.
        // Also strip any leading/trailing GAPs so the lane starts and ends on real sequenced records.
        if let Some(first_non_gap_idx) = kept_records.iter().position(|r| r.kind != KIND_GAP) {
            let last_non_gap_idx = kept_records.iter().rposition(|r| r.kind != KIND_GAP).unwrap();
            for rec in &kept_records[..first_non_gap_idx] {
                kept_record_keys.remove(&(inc, rec.seq));
            }
            for rec in &kept_records[last_non_gap_idx + 1..] {
                kept_record_keys.remove(&(inc, rec.seq));
            }
            kept_records = kept_records[first_non_gap_idx..=last_non_gap_idx].to_vec();
        } else {
            for rec in &kept_records {
                kept_record_keys.remove(&(inc, rec.seq));
            }
            kept_records.clear();
        }

        let writer_written = lane.written;
        if kept_records.is_empty() {
            export_lanes.insert(
                inc,
                ExportLane {
                    incarnation: inc,
                    first_seq: 0,
                    last_seq: 0,
                    kept: 0,
                    dropped_by_export: writer_written,
                },
            );
            records_dropped_total += writer_written;
        } else {
            let first_seq = kept_records.first().unwrap().seq;
            let last_seq = kept_records.last().unwrap().seq;
            let kept = kept_records.iter().filter(|r| r.kind != KIND_GAP).count() as u64;
            let dropped_by_export = writer_written - kept;

            export_lanes.insert(
                inc,
                ExportLane {
                    incarnation: inc,
                    first_seq,
                    last_seq,
                    kept,
                    dropped_by_export,
                },
            );
            records_kept_total += kept;
            records_dropped_total += dropped_by_export;
        }
    }

    // Count kept and dropped for AUTO ops and FIFO events
    let mut ops_kept = 0u64;
    let mut ops_dropped = 0u64;
    let mut events_kept = 0u64;
    let mut events_dropped = 0u64;
    let mut ring = ExportRing { first_generation: 0, last_generation: 0, generations: 0, records_kept: 0,
        records_dropped: 0, source_last_generation: 0 };

    for chunk in &raw_chunks {
        match chunk.ty {
            CHUNK_AUTO_OPS if chunk.payload.len() >= 16 => {
                let count = u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                let width = u32::from_le_bytes(chunk.payload[12..16].try_into().unwrap()) as usize;
                let actual_width = if width == OP_BYTES_BUILD as usize || width == OP_BYTES as usize {
                    width
                } else {
                    op_width
                };

                for i in 0..count {
                    let off = 16 + i * actual_width;
                    if off + actual_width <= chunk.payload.len() {
                        let flags =
                            u16::from_le_bytes(chunk.payload[off + 14..off + 16].try_into().unwrap());
                        let frame =
                            u64::from_le_bytes(chunk.payload[off + 16..off + 24].try_into().unwrap());
                        let has_token = flags & OP_TOKEN != 0 && range.contains(&frame);
                        let has_build = if actual_width == OP_BYTES_BUILD as usize {
                            let b_frame = u64::from_le_bytes(
                                chunk.payload[off + 128..off + 136].try_into().unwrap(),
                            );
                            let b_flags = u16::from_le_bytes(
                                chunk.payload[off + 140..off + 142].try_into().unwrap(),
                            );
                            (b_flags & OP_BUILD_KNOWN != 0) && range.contains(&b_frame)
                        } else {
                            false
                        };

                        if has_token || has_build {
                            ops_kept += 1;
                        } else {
                            ops_dropped += 1;
                        }
                        // RING records: a block shares one frame token, so a generation is kept or dropped whole
                        if ringed && flags & OP_RING != 0 && actual_width == op_width {
                            let generation =
                                u32::from_le_bytes(chunk.payload[off + 160..off + 164].try_into().unwrap());
                            ring.source_last_generation = ring.source_last_generation.max(generation);
                            if has_token || has_build {
                                if ring.records_kept == 0 {
                                    ring.first_generation = generation;
                                }
                                if ring.records_kept == 0 || generation != ring.last_generation {
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
            }
            CHUNK_FIFO_EVENTS if chunk.payload.len() >= 16 => {
                let count = u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                let actual_stride = 96;

                for i in 0..count {
                    let off = 16 + i * actual_stride;
                    if off + actual_stride <= chunk.payload.len() {
                        let flags =
                            u16::from_le_bytes(chunk.payload[off + 38..off + 40].try_into().unwrap());
                        let frame_id =
                            u64::from_le_bytes(chunk.payload[off + 48..off + 56].try_into().unwrap());
                        if flags & EV_TOKEN_PRESENT != 0 && range.contains(&frame_id) {
                            events_kept += 1;
                        } else {
                            events_dropped += 1;
                        }
                    }
                }
            }
            _ => {}
        }
    }

    let source_auto_frames = cap
        .auto
        .as_ref()
        .and_then(|a| a.last_status.as_ref())
        .map_or(0, |s| s.counters[5]);
    let source_fifo_last_attempt_seq = cap
        .fifo
        .as_ref()
        .and_then(|f| f.last_status.as_ref())
        .map_or(0, |s| s.last_attempt_seq);

    // Build the 112-byte EXPORT header + per-lane 40-byte entries
    let lane_count = export_lanes.len();
    let total_export_len = EXPORT_HEADER_LEN + lane_count * EXPORT_LANE_ENTRY_LEN + if ringed { EXPORT_RING_LEN } else { 0 };
    let mut export_payload = Vec::with_capacity(total_export_len);

    let flags = if to_end_of_run { EXPORT_FLAG_TO_END } else { 0 } | if ringed { EXPORT_FLAG_RING } else { 0 };
    export_payload.extend_from_slice(&EXPORT_SCHEMA.to_le_bytes()); // schema: u16
    export_payload.extend_from_slice(&flags.to_le_bytes());         // flags: u16
    export_payload.extend_from_slice(&crc32(bytes).to_le_bytes()); // source_crc: u32
    export_payload.extend_from_slice(&(bytes.len() as u64).to_le_bytes()); // source_size: u64
    export_payload.extend_from_slice(&(*range.start()).to_le_bytes()); // frame_first: u64
    export_payload.extend_from_slice(&(*range.end()).to_le_bytes()); // frame_last: u64
    export_payload.extend_from_slice(&clipped_zones.to_le_bytes());
    export_payload.extend_from_slice(&records_kept_total.to_le_bytes());
    export_payload.extend_from_slice(&records_dropped_total.to_le_bytes());
    export_payload.extend_from_slice(&ops_kept.to_le_bytes());
    export_payload.extend_from_slice(&ops_dropped.to_le_bytes());
    export_payload.extend_from_slice(&events_kept.to_le_bytes());
    export_payload.extend_from_slice(&events_dropped.to_le_bytes());
    export_payload.extend_from_slice(&source_fifo_last_attempt_seq.to_le_bytes());
    export_payload.extend_from_slice(&source_auto_frames.to_le_bytes());
    export_payload.extend_from_slice(&(lane_count as u32).to_le_bytes());
    export_payload.extend_from_slice(&0u32.to_le_bytes()); // reserved: u32

    for el in export_lanes.values() {
        export_payload.extend_from_slice(&el.incarnation.to_le_bytes());
        export_payload.extend_from_slice(&0u32.to_le_bytes()); // reserved: u32
        export_payload.extend_from_slice(&el.first_seq.to_le_bytes());
        export_payload.extend_from_slice(&el.last_seq.to_le_bytes());
        export_payload.extend_from_slice(&el.kept.to_le_bytes());
        export_payload.extend_from_slice(&el.dropped_by_export.to_le_bytes());
    }
    if ringed {
        export_payload.extend_from_slice(&ring.first_generation.to_le_bytes());
        export_payload.extend_from_slice(&ring.last_generation.to_le_bytes());
        export_payload.extend_from_slice(&ring.generations.to_le_bytes());
        export_payload.extend_from_slice(&ring.records_kept.to_le_bytes());
        export_payload.extend_from_slice(&ring.records_dropped.to_le_bytes());
        export_payload.extend_from_slice(&ring.source_last_generation.to_le_bytes());
        export_payload.extend_from_slice(&0u32.to_le_bytes()); // reserved: u32
    }

    // 7. Pass 2: Filter and serialize chunks.
    // Placement rule: EXPORT is emitted strictly right after HEADER/CLOCKS (and PROFILE if present).
    let mut emitted_chunks: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut prev_emitted_seq: BTreeMap<u32, u64> = BTreeMap::new();

    for chunk in &raw_chunks {
        match chunk.ty {
            CHUNK_HEADER => {
                emitted_chunks.push((CHUNK_HEADER, chunk.payload.to_vec()));
            }
            CHUNK_CLOCKS => {
                emitted_chunks.push((CHUNK_CLOCKS, chunk.payload.to_vec()));
                if !cap.profile_flag {
                    emitted_chunks.push((CHUNK_EXPORT, export_payload.clone()));
                }
            }
            crate::profile::CHUNK_PROFILE => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
                if cap.profile_flag {
                    emitted_chunks.push((CHUNK_EXPORT, export_payload.clone()));
                }
            }

            CHUNK_DESCRIPTORS | CHUNK_THREADS
            | crate::waits::CHUNK_WAIT_CONFIG
            | crate::func_paths::CHUNK_FUNC_CONFIG | crate::counters::CHUNK_COUNTERS
            | crate::source_checkpoints::CHUNK_ADMISSION | crate::source_checkpoints::CHUNK_SOURCE_SEAL
            | crate::fifo::CHUNK_FIFO_CONFIG | crate::auto_rsx::CHUNK_AUTO_CONFIG | crate::auto_rsx::CHUNK_AUTO_CLOCK => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }

            // Domain extension declarations (types 8..=18)
            t if (crate::ext::CHUNK_DOMAIN..=crate::ext::CHUNK_DOMAIN_FINAL).contains(&t)
                || t == crate::ext::CHUNK_SPU_ORDER =>
            {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }

            CHUNK_AUTO_COVERAGE if chunk.payload.len() >= 24 => {
                let frame = u64::from_le_bytes(chunk.payload[16..24].try_into().unwrap());
                if range.contains(&frame) {
                    emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
                }
            }

            CHUNK_AUTO_OPS if chunk.payload.len() >= 16 => {
                let schema = u16::from_le_bytes(chunk.payload[0..2].try_into().unwrap());
                let reserved = u16::from_le_bytes(chunk.payload[2..4].try_into().unwrap());
                let instance = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap());
                let count = u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                let width = u32::from_le_bytes(chunk.payload[12..16].try_into().unwrap()) as usize;
                let actual_width = if width == OP_BYTES_BUILD as usize || width == OP_BYTES as usize {
                    width
                } else {
                    op_width
                };

                let mut kept_ops = Vec::new();
                for i in 0..count {
                    let off = 16 + i * actual_width;
                    if off + actual_width <= chunk.payload.len() {
                        let flags =
                            u16::from_le_bytes(chunk.payload[off + 14..off + 16].try_into().unwrap());
                        let frame =
                            u64::from_le_bytes(chunk.payload[off + 16..off + 24].try_into().unwrap());
                        let has_token = flags & OP_TOKEN != 0 && range.contains(&frame);
                        let has_build = if actual_width == OP_BYTES_BUILD as usize {
                            let b_frame =
                                u64::from_le_bytes(chunk.payload[off + 128..off + 136].try_into().unwrap());
                            let b_flags =
                                u16::from_le_bytes(chunk.payload[off + 140..off + 142].try_into().unwrap());
                            (b_flags & OP_BUILD_KNOWN != 0) && range.contains(&b_frame)
                        } else {
                            false
                        };

                        if has_token || has_build {
                            kept_ops.extend_from_slice(&chunk.payload[off..off + actual_width]);
                        }
                    }
                }

                if !kept_ops.is_empty() {
                    let mut new_payload = Vec::with_capacity(16 + kept_ops.len());
                    new_payload.extend_from_slice(&schema.to_le_bytes());
                    new_payload.extend_from_slice(&reserved.to_le_bytes());
                    new_payload.extend_from_slice(&instance.to_le_bytes());
                    let new_count = (kept_ops.len() / actual_width) as u32;
                    new_payload.extend_from_slice(&new_count.to_le_bytes());
                    new_payload.extend_from_slice(&(actual_width as u32).to_le_bytes());
                    new_payload.extend_from_slice(&kept_ops);
                    emitted_chunks.push((CHUNK_AUTO_OPS, new_payload));
                }
            }

            // Keep STATUS chunks as written! (Evidence preservation)
            CHUNK_AUTO_STATUS => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }

            CHUNK_FIFO_STATUS => {
                if chunk.payload.len() >= 4 {
                    let flags = u16::from_le_bytes(chunk.payload[2..4].try_into().unwrap());
                    if flags & crate::fifo::ST_FINAL != 0 {
                        emitted_chunks.push((CHUNK_FIFO_STATUS, chunk.payload.to_vec()));
                    }
                }
            }

            CHUNK_FIFO_EVENTS if chunk.payload.len() >= 16 => {
                let schema = u16::from_le_bytes(chunk.payload[0..2].try_into().unwrap());
                let reserved = u16::from_le_bytes(chunk.payload[2..4].try_into().unwrap());
                let instance = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap());
                let count = u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                let actual_stride = 96;

                let mut kept_events = Vec::new();
                for i in 0..count {
                    let off = 16 + i * actual_stride;
                    if off + actual_stride <= chunk.payload.len() {
                        let flags =
                            u16::from_le_bytes(chunk.payload[off + 38..off + 40].try_into().unwrap());
                        let frame_id =
                            u64::from_le_bytes(chunk.payload[off + 48..off + 56].try_into().unwrap());
                        if flags & EV_TOKEN_PRESENT != 0 && range.contains(&frame_id) {
                            kept_events.extend_from_slice(&chunk.payload[off..off + actual_stride]);
                        }
                    }
                }

                if !kept_events.is_empty() {
                    let mut new_payload = Vec::with_capacity(16 + kept_events.len());
                    new_payload.extend_from_slice(&schema.to_le_bytes());
                    new_payload.extend_from_slice(&reserved.to_le_bytes());
                    new_payload.extend_from_slice(&instance.to_le_bytes());
                    let new_count = (kept_events.len() / actual_stride) as u32;
                    new_payload.extend_from_slice(&new_count.to_le_bytes());
                    new_payload.extend_from_slice(&0u32.to_le_bytes()); // z1 (reserved)
                    new_payload.extend_from_slice(&kept_events);
                    emitted_chunks.push((CHUNK_FIFO_EVENTS, new_payload));
                }
            }

            CHUNK_RECORDS if chunk.payload.len() >= 8 => {
                let inc = u32::from_le_bytes(chunk.payload[0..4].try_into().unwrap());
                let count = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
                let mut chunk_out = Vec::new();
                for i in 0..count {
                    let off = 8 + i * RECORD_LEN;
                    if off + RECORD_LEN <= chunk.payload.len() {
                        let rec = RawRecord::decode(&chunk.payload[off..off + RECORD_LEN]);
                        if kept_record_keys.contains(&(inc, rec.seq)) {
                            let mut rec_to_emit = rec;
                            if let Some(&prev) = prev_emitted_seq.get(&inc) {
                                if rec.seq > prev + 1 {
                                    if rec.kind == KIND_GAP {
                                        rec_to_emit.seq = prev + 1;
                                        rec_to_emit.b = prev + 1;
                                    } else if chunk_out.len() >= RECORD_LEN
                                        && chunk_out[chunk_out.len() - RECORD_LEN] == KIND_GAP
                                    {
                                        let last_start = chunk_out.len() - RECORD_LEN;
                                        chunk_out[last_start + 24..last_start + 32]
                                            .copy_from_slice(&(rec.seq - 1).to_le_bytes());
                                    } else {
                                        chunk_out.extend_from_slice(&encode_gap(prev + 1, rec.seq - 1, true));
                                    }
                                }
                            }
                            chunk_out.extend_from_slice(&encode_record(&rec_to_emit));
                            let end_seq = if rec_to_emit.kind == KIND_GAP { rec_to_emit.c } else { rec_to_emit.seq };
                            prev_emitted_seq.insert(inc, end_seq);
                        }
                    }
                }
                if !chunk_out.is_empty() {
                    let new_count = (chunk_out.len() / RECORD_LEN) as u32;
                    let mut payload = Vec::with_capacity(8 + chunk_out.len());
                    payload.extend_from_slice(&inc.to_le_bytes());
                    payload.extend_from_slice(&new_count.to_le_bytes());
                    payload.extend_from_slice(&chunk_out);
                    emitted_chunks.push((CHUNK_RECORDS, payload));
                }
            }

            // Keep RETIRED and TRAILER as written by the original writer!
            CHUNK_RETIRED | CHUNK_TRAILER => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }

            _ => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }
        }
    }

    // 8. Re-sequence chunks and serialize with CRCs.
    let mut out = Vec::new();
    out.extend_from_slice(&bytes[0..16]); // Header preamble (magic, versions, flags)

    for (seq, (ty, payload)) in emitted_chunks.iter().enumerate() {
        let chunk_seq = seq as u64;
        let len = payload.len() as u32;
        let mut chunk_bytes = Vec::with_capacity(16 + payload.len() + 4);
        chunk_bytes.extend_from_slice(&ty.to_le_bytes());
        chunk_bytes.extend_from_slice(&len.to_le_bytes());
        chunk_bytes.extend_from_slice(&chunk_seq.to_le_bytes());
        chunk_bytes.extend_from_slice(payload);

        let crc = crc32(&chunk_bytes);
        chunk_bytes.extend_from_slice(&crc.to_le_bytes());

        out.extend_from_slice(&chunk_bytes);
    }

    // 9. Integrity self-validation before reporting success.
    crate::reader::read(&out)
        .map_err(|e| ExportError::Malformed(format!("exported capture failed integrity check: {e}")))?;

    Ok(out)
}

/// Export one frame or a range of frames from an input capture file to an output capture file.
pub fn export_capture<P: AsRef<Path>, Q: AsRef<Path>>(
    in_path: P,
    out_path: Q,
    range: RangeInclusive<u64>,
) -> Result<(), ExportError> {
    let bytes = std::fs::read(in_path.as_ref()).map_err(ExportError::Io)?;
    let exported = export_bytes(&bytes, range)?;
    let out = out_path.as_ref();
    let tmp = match out.file_name() {
        Some(name) => {
            let mut tmp_name = name.to_os_string();
            tmp_name.push(format!(".tmp-{}", std::process::id()));
            out.with_file_name(tmp_name)
        }
        None => out.with_extension(format!("tmp-{}", std::process::id())),
    };
    if let Err(e) = std::fs::write(&tmp, exported) {
        let _ = std::fs::remove_file(&tmp);
        return Err(ExportError::Io(e));
    }
    if let Err(e) = std::fs::rename(&tmp, out) {
        let _ = std::fs::remove_file(&tmp);
        return Err(ExportError::Io(e));
    }
    Ok(())
}

/// Export like `export_capture`, but never replace or truncate any existing file: the viewer's "save window / save
/// range" path.  The export is written to a temporary created exclusively next to `out_path` (a pre-existing file of
/// that name is skipped, never truncated), then published with a hard link, which fails instead of replacing when
/// `out_path` exists at that moment.  On a filesystem without hard links the target itself is created exclusively and
/// written.  Either way a destination that appears at any point before publication is left untouched and the call
/// fails with `ErrorKind::AlreadyExists`; only the temporary this call created is removed.
pub fn export_capture_no_clobber<P: AsRef<Path>, Q: AsRef<Path>>(
    in_path: P,
    out_path: Q,
    range: RangeInclusive<u64>,
) -> Result<(), ExportError> {
    let bytes = std::fs::read(in_path.as_ref()).map_err(ExportError::Io)?;
    export_bytes_no_clobber(&bytes, out_path, range)
}

/// Export an immutable capture snapshot without replacing an existing destination.
/// Live consumers pass exactly the reader-accepted prefix, not later received bytes.
/// Validation finishes before any output file is created.
pub fn export_bytes_no_clobber<Q: AsRef<Path>>(
    bytes: &[u8],
    out_path: Q,
    range: RangeInclusive<u64>,
) -> Result<(), ExportError> {
    let exported = export_bytes(bytes, range)?;
    publish_no_clobber(&exported, out_path.as_ref(), || {})
}

fn already_exists(out: &Path) -> ExportError {
    ExportError::Io(std::io::Error::new(
        std::io::ErrorKind::AlreadyExists,
        format!("{} already exists; nothing was written", out.display()),
    ))
}

/// Writes `bytes` to `out` without ever replacing an existing file.  `before_publish` runs after the temporary is
/// complete and immediately before the no-replace publication (tests use it to create the destination at that boundary).
fn publish_no_clobber(bytes: &[u8], out: &Path, before_publish: impl FnOnce()) -> Result<(), ExportError> {
    use std::io::Write;
    let name = out
        .file_name()
        .ok_or_else(|| ExportError::Usage(format!("{}: the output path has no file name", out.display())))?
        .to_os_string();
    // Exclusive temporary creation: an existing file at a candidate name belongs to someone else and is skipped.
    let mut n = 0u32;
    let (tmp, mut file) = loop {
        let mut t = name.clone();
        t.push(format!(".tmp-{}-{n}", std::process::id()));
        let p = out.with_file_name(t);
        match std::fs::OpenOptions::new().write(true).create_new(true).open(&p) {
            Ok(f) => break (p, f),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists && n < 1024 => n += 1,
            Err(e) => return Err(ExportError::Io(e)),
        }
    };
    let written = file.write_all(bytes).and_then(|()| file.sync_all());
    drop(file);
    if let Err(e) = written {
        let _ = std::fs::remove_file(&tmp);
        return Err(ExportError::Io(e));
    }
    before_publish();
    let result = match std::fs::hard_link(&tmp, out) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Err(already_exists(out)),
        // No hard links here (e.g. FAT): create the target itself exclusively; remove it only if this call created it.
        Err(_) => match std::fs::OpenOptions::new().write(true).create_new(true).open(out) {
            Ok(mut f) => {
                let w = f.write_all(bytes).and_then(|()| f.sync_all());
                drop(f);
                match w {
                    Ok(()) => Ok(()),
                    Err(e) => {
                        let _ = std::fs::remove_file(out);
                        Err(ExportError::Io(e))
                    }
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Err(already_exists(out)),
            Err(e) => Err(ExportError::Io(e)),
        },
    };
    let _ = std::fs::remove_file(&tmp);
    result
}

#[cfg(test)]
mod no_clobber_tests {
    use super::*;

    fn dir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("ps3dk-prof-noclobber-{}-{tag}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }
    fn names(d: &Path) -> Vec<String> {
        let mut v: Vec<String> = std::fs::read_dir(d).unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
        v.sort();
        v
    }

    #[test]
    fn no_clobber_refuses_a_destination_that_appears_at_publication() {
        let d = dir("race");
        let out = d.join("cap.0..1.prof");
        let r = publish_no_clobber(b"ours", &out, || std::fs::write(&out, b"theirs").unwrap());
        match r {
            Err(ExportError::Io(e)) => assert_eq!(e.kind(), std::io::ErrorKind::AlreadyExists, "{e}"),
            other => panic!("expected AlreadyExists, got {other:?}"),
        }
        assert_eq!(std::fs::read(&out).unwrap(), b"theirs", "the destination that appeared is untouched");
        assert_eq!(names(&d), vec!["cap.0..1.prof".to_string()], "only this call's temporary was removed");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn no_clobber_refuses_an_existing_destination() {
        let d = dir("exists");
        let out = d.join("cap.window_2.prof");
        std::fs::write(&out, b"theirs").unwrap();
        assert!(publish_no_clobber(b"ours", &out, || {}).is_err());
        assert_eq!(std::fs::read(&out).unwrap(), b"theirs");
        assert_eq!(names(&d), vec!["cap.window_2.prof".to_string()]);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn no_clobber_preserves_precreated_temporaries() {
        let d = dir("tmp");
        let out = d.join("cap.3..5.prof");
        let pid = std::process::id();
        let old_style = d.join(format!("cap.3..5.prof.tmp-{pid}"));
        let first_candidate = d.join(format!("cap.3..5.prof.tmp-{pid}-0"));
        std::fs::write(&old_style, b"keep-a").unwrap();
        std::fs::write(&first_candidate, b"keep-b").unwrap();
        publish_no_clobber(b"ours", &out, || {}).unwrap();
        assert_eq!(std::fs::read(&out).unwrap(), b"ours");
        assert_eq!(std::fs::read(&old_style).unwrap(), b"keep-a", "pre-existing temporary not truncated");
        assert_eq!(std::fs::read(&first_candidate).unwrap(), b"keep-b", "pre-existing temporary not truncated");
        assert_eq!(names(&d).len(), 3, "no temporary of this call is left behind: {:?}", names(&d));
        let _ = std::fs::remove_dir_all(&d);
    }
}

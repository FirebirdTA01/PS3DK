//! Standalone capture frame export: one frame or a frame range saved as an independent .prof capture.

use std::collections::{BTreeMap, BTreeSet};
use std::ops::RangeInclusive;
use std::path::Path;

use crate::auto_rsx::{
    CHUNK_AUTO_COVERAGE, CHUNK_AUTO_OPS, CHUNK_AUTO_STATUS,
    OP_BYTES, OP_BYTES_BUILD, OP_BUILD_KNOWN, OP_TOKEN,
};
use crate::fifo::{CHUNK_FIFO_EVENTS, CHUNK_FIFO_STATUS, EV_TOKEN_PRESENT};
use crate::hash::crc32;
use crate::model::*;
use crate::reader::RawRecord;

#[derive(Debug)]
pub enum ExportError {
    Io(std::io::Error),
    Malformed(&'static str),
    FrameNotFound(u64),
    Usage(String),
}

impl std::fmt::Display for ExportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ExportError::Io(e) => write!(f, "{e}"),
            ExportError::Malformed(why) => write!(f, "MALFORMED: {why}"),
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

fn encode_gap(start_seq: u64, end_seq: u64) -> [u8; 32] {
    let mut buf = [0u8; 32];
    buf[0] = KIND_GAP;
    buf[8..16].copy_from_slice(&start_seq.to_le_bytes());
    buf[16..24].copy_from_slice(&start_seq.to_le_bytes());
    buf[24..32].copy_from_slice(&end_seq.to_le_bytes());
    buf
}

/// Export one frame or a range of frames from capture bytes into standalone capture bytes.
pub fn export_bytes(bytes: &[u8], range: RangeInclusive<u64>) -> Result<Vec<u8>, ExportError> {
    if bytes.len() < 16 {
        return Err(ExportError::Malformed("capture header shorter than 16 bytes"));
    }
    if &bytes[0..8] != b"PS3DKPRF" {
        return Err(ExportError::Malformed("bad magic"));
    }
    let major = u16::from_le_bytes(bytes[8..10].try_into().unwrap());
    if major != 1 {
        return Err(ExportError::Malformed("unsupported major version"));
    }

    // 1. Structural parse of the source capture using the reader.
    let cap = crate::reader::read(bytes).map_err(|_| ExportError::Malformed("source capture malformed"))?;

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
    let op_width = cap
        .auto
        .as_ref()
        .and_then(|a| a.config.as_ref())
        .and_then(|c| c.attached())
        .map_or(OP_BYTES, |att| att.op_width()) as usize;

    // 4. Determine frame owner lane and time bounds.
    let owner_lane = cap
        .lanes
        .values()
        .find(|l| l.frames.iter().any(|fr| fr.id == *range.start()))
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
    let mut clipped_span_ids: BTreeSet<(u32, u64)> = BTreeSet::new();
    for span in &cap.spans {
        let crosses_left =
            span.begin_tb < tb_start && span.end_tb.map_or(true, |etb| etb >= tb_start);
        let crosses_right = if let Some(etb) = tb_end {
            span.begin_tb <= etb && span.end_tb.map_or(true, |end_tb| end_tb > etb)
        } else {
            false
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
            return Err(ExportError::Malformed("truncated chunk header"));
        }
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if bytes.len() - pos < 16 + len + 4 {
            return Err(ExportError::Malformed("truncated chunk payload"));
        }
        let payload = &bytes[pos + 16..pos + 16 + len];
        let file_crc =
            u32::from_le_bytes(bytes[pos + 16 + len..pos + 20 + len].try_into().unwrap());
        let computed_crc = crc32(&bytes[pos..pos + 16 + len]);
        if file_crc != computed_crc {
            return Err(ExportError::Malformed("chunk CRC mismatch"));
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
    let mut output_records_per_lane: BTreeMap<u32, Vec<u8>> = BTreeMap::new();
    let mut records_kept_total = 0u64;
    let mut records_dropped_total = 0u64;

    for (&inc, lane) in &cap.lanes {
        let is_owner = lane.is_frame_owner();
        let all_recs = raw_lane_records.get(&inc).map(|v| v.as_slice()).unwrap_or(&[]);

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

            let in_window = if is_owner {
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

            if in_window && !is_clipped && rec.kind != KIND_GAP {
                kept_records.push(*rec);
            }
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
            let kept = kept_records.len() as u64;
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

            // Reconstruct lane records, inserting KIND_GAP for any missing sequences between kept records
            let mut out_bytes = Vec::new();
            for (i, rec) in kept_records.iter().enumerate() {
                if i > 0 {
                    let prev_seq = kept_records[i - 1].seq;
                    if rec.seq > prev_seq + 1 {
                        out_bytes.extend_from_slice(&encode_gap(prev_seq + 1, rec.seq - 1));
                    }
                }
                out_bytes.extend_from_slice(&encode_record(rec));
            }

            let count = (out_bytes.len() / RECORD_LEN) as u32;
            let mut payload = Vec::with_capacity(8 + out_bytes.len());
            payload.extend_from_slice(&inc.to_le_bytes());
            payload.extend_from_slice(&count.to_le_bytes());
            payload.extend_from_slice(&out_bytes);
            output_records_per_lane.insert(inc, payload);
        }
    }

    // Count kept and dropped for AUTO ops and FIFO events
    let mut ops_kept = 0u64;
    let mut ops_dropped = 0u64;
    let mut events_kept = 0u64;
    let mut events_dropped = 0u64;

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
    let total_export_len = EXPORT_HEADER_LEN + lane_count * EXPORT_LANE_ENTRY_LEN;
    let mut export_payload = Vec::with_capacity(total_export_len);

    let flags = if to_end_of_run { EXPORT_FLAG_TO_END } else { 0 };
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

    // 7. Pass 2: Filter and serialize chunks.
    // Placement rule: EXPORT is emitted strictly right after HEADER/CLOCKS (and PROFILE if present).
    let mut emitted_chunks: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut records_emitted = false;

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
            CHUNK_AUTO_STATUS | CHUNK_FIFO_STATUS => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
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

            CHUNK_RECORDS => {
                if !records_emitted {
                    for payload in output_records_per_lane.values() {
                        emitted_chunks.push((CHUNK_RECORDS, payload.clone()));
                    }
                    records_emitted = true;
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
    std::fs::write(out_path.as_ref(), exported).map_err(ExportError::Io)?;
    Ok(())
}


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

    // 4. Determine interval bounds per thread.
    // For each frame owner lane: keep from mark A (r.c == A) to mark B+1 (r.c == B+1) inclusive.
    // If B+1 does not exist in the run, keep from mark A to end of thread.
    struct ThreadInterval {
        seq_start: u64,
        seq_end: u64,
    }

    let mut thread_intervals: BTreeMap<u32, ThreadInterval> = BTreeMap::new();
    let mut global_tb_start: Option<u64> = None;
    let mut global_tb_end: Option<u64> = None;

    for (&inc, lane) in &cap.lanes {
        if !lane.frames.is_empty() {
            let mark_a = lane.frames.iter().find(|fr| fr.id == *range.start());
            let mark_b1 = lane.frames.iter().find(|fr| fr.id == *range.end() + 1);
            if let Some(a) = mark_a {
                if global_tb_start.is_none() || a.tb < global_tb_start.unwrap() {
                    global_tb_start = Some(a.tb);
                }
                let seq_end = if let Some(b1) = mark_b1 {
                    if global_tb_end.is_none() || b1.tb > global_tb_end.unwrap() {
                        global_tb_end = Some(b1.tb);
                    }
                    b1.seq
                } else {
                    u64::MAX
                };
                thread_intervals.insert(
                    inc,
                    ThreadInterval {
                        seq_start: a.seq,
                        seq_end,
                    },
                );
            }
        }
    }

    // 5. Linear walk to parse raw chunks from bytes.
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

    // 6. Pass 1: Count kept and dropped items for EXPORT chunk.
    let mut records_kept = 0u64;
    let mut records_dropped = 0u64;
    let mut records_kept_per_lane: BTreeMap<u32, u64> = BTreeMap::new();

    let mut ops_kept = 0u64;
    let mut ops_dropped = 0u64;

    let mut events_kept = 0u64;
    let mut events_dropped = 0u64;

    for chunk in &raw_chunks {
        match chunk.ty {
            CHUNK_RECORDS if chunk.payload.len() >= 8 => {
                let inc = u32::from_le_bytes(chunk.payload[0..4].try_into().unwrap());
                let n = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
                let interval = thread_intervals.get(&inc);

                for i in 0..n {
                    let off = 8 + i * RECORD_LEN;
                    if off + RECORD_LEN <= chunk.payload.len() {
                        let rec = RawRecord::decode(&chunk.payload[off..off + RECORD_LEN]);
                        let keep = if let Some(iv) = interval {
                            rec.seq >= iv.seq_start && rec.seq <= iv.seq_end
                        } else if let (Some(tb0), Some(tb1)) = (global_tb_start, global_tb_end) {
                            rec.b >= tb0 && rec.b <= tb1
                        } else if let Some(tb0) = global_tb_start {
                            rec.b >= tb0
                        } else {
                            false
                        };

                        if keep {
                            records_kept += 1;
                            *records_kept_per_lane.entry(inc).or_insert(0) += 1;
                        } else {
                            records_dropped += 1;
                        }
                    }
                }
            }
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
                        let flags = u16::from_le_bytes(chunk.payload[off + 14..off + 16].try_into().unwrap());
                        let frame = u64::from_le_bytes(chunk.payload[off + 16..off + 24].try_into().unwrap());
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
                            ops_kept += 1;
                        } else {
                            ops_dropped += 1;
                        }
                    }
                }
            }
            CHUNK_FIFO_EVENTS if chunk.payload.len() >= 16 => {
                let count = u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                let stride = u32::from_le_bytes(chunk.payload[12..16].try_into().unwrap()) as usize;
                let actual_stride = if stride == 64 { 64 } else { 64 };

                for i in 0..count {
                    let off = 16 + i * actual_stride;
                    if off + actual_stride <= chunk.payload.len() {
                        let flags = u16::from_le_bytes(chunk.payload[off + 10..off + 12].try_into().unwrap());
                        let frame_id =
                            u64::from_le_bytes(chunk.payload[off + 40..off + 48].try_into().unwrap());
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

    // Build the 72-byte EXPORT chunk payload.
    let mut export_payload = Vec::with_capacity(EXPORT_PAYLOAD_LEN);
    export_payload.extend_from_slice(&EXPORT_SCHEMA.to_le_bytes()); // schema: u16
    export_payload.extend_from_slice(&0u16.to_le_bytes());          // reserved: u16
    export_payload.extend_from_slice(&crc32(bytes).to_le_bytes());  // source_crc: u32
    export_payload.extend_from_slice(&(bytes.len() as u64).to_le_bytes()); // source_size: u64
    export_payload.extend_from_slice(&(*range.start()).to_le_bytes());     // frame_first: u64
    export_payload.extend_from_slice(&(*range.end()).to_le_bytes());       // frame_last: u64
    export_payload.extend_from_slice(&records_kept.to_le_bytes());
    export_payload.extend_from_slice(&records_dropped.to_le_bytes());
    export_payload.extend_from_slice(&ops_kept.to_le_bytes());
    export_payload.extend_from_slice(&ops_dropped.to_le_bytes());
    export_payload.extend_from_slice(&events_kept.to_le_bytes());
    export_payload.extend_from_slice(&events_dropped.to_le_bytes());

    // 7. Pass 2: Filter and serialize chunks.
    let mut emitted_chunks: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut export_emitted = false;

    for chunk in &raw_chunks {
        // Emit CHUNK_EXPORT after preamble (before event data chunks, or right after threads/clocks/configs).
        if !export_emitted
            && (chunk.ty == CHUNK_RECORDS
                || chunk.ty == CHUNK_AUTO_OPS
                || chunk.ty == CHUNK_FIFO_EVENTS
                || chunk.ty == CHUNK_TRAILER)
        {
            emitted_chunks.push((CHUNK_EXPORT, export_payload.clone()));
            export_emitted = true;
        }

        match chunk.ty {
            CHUNK_HEADER | CHUNK_CLOCKS | CHUNK_DESCRIPTORS | CHUNK_THREADS
            | crate::profile::CHUNK_PROFILE | crate::waits::CHUNK_WAIT_CONFIG
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
                let stride = u32::from_le_bytes(chunk.payload[12..16].try_into().unwrap()) as usize;
                let actual_stride = if stride == 64 { 64 } else { 64 };

                let mut kept_events = Vec::new();
                for i in 0..count {
                    let off = 16 + i * actual_stride;
                    if off + actual_stride <= chunk.payload.len() {
                        let flags =
                            u16::from_le_bytes(chunk.payload[off + 10..off + 12].try_into().unwrap());
                        let frame_id =
                            u64::from_le_bytes(chunk.payload[off + 40..off + 48].try_into().unwrap());
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
                    new_payload.extend_from_slice(&(actual_stride as u32).to_le_bytes());
                    new_payload.extend_from_slice(&kept_events);
                    emitted_chunks.push((CHUNK_FIFO_EVENTS, new_payload));
                }
            }

            CHUNK_RECORDS if chunk.payload.len() >= 8 => {
                let inc = u32::from_le_bytes(chunk.payload[0..4].try_into().unwrap());
                let n = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
                let interval = thread_intervals.get(&inc);

                let mut kept_recs = Vec::new();
                for i in 0..n {
                    let off = 8 + i * RECORD_LEN;
                    if off + RECORD_LEN <= chunk.payload.len() {
                        let rec = RawRecord::decode(&chunk.payload[off..off + RECORD_LEN]);
                        let keep = if let Some(iv) = interval {
                            rec.seq >= iv.seq_start && rec.seq <= iv.seq_end
                        } else if let (Some(tb0), Some(tb1)) = (global_tb_start, global_tb_end) {
                            rec.b >= tb0 && rec.b <= tb1
                        } else if let Some(tb0) = global_tb_start {
                            rec.b >= tb0
                        } else {
                            false
                        };

                        if keep {
                            kept_recs.extend_from_slice(&chunk.payload[off..off + RECORD_LEN]);
                        }
                    }
                }

                if !kept_recs.is_empty() {
                    let mut new_payload = Vec::with_capacity(8 + kept_recs.len());
                    new_payload.extend_from_slice(&inc.to_le_bytes());
                    let new_count = (kept_recs.len() / RECORD_LEN) as u32;
                    new_payload.extend_from_slice(&new_count.to_le_bytes());
                    new_payload.extend_from_slice(&kept_recs);
                    emitted_chunks.push((CHUNK_RECORDS, new_payload));
                }
            }

            CHUNK_RETIRED if chunk.payload.len() >= 4 => {
                let count = u32::from_le_bytes(chunk.payload[0..4].try_into().unwrap()) as usize;
                let mut new_payload = chunk.payload.to_vec();
                for i in 0..count {
                    let off = 4 + i * FINAL_ENTRY_LEN;
                    if off + FINAL_ENTRY_LEN <= new_payload.len() {
                        let inc = u32::from_le_bytes(new_payload[off..off + 4].try_into().unwrap());
                        let kept = records_kept_per_lane.get(&inc).copied().unwrap_or(0);
                        let dropped =
                            u64::from_le_bytes(new_payload[off + 24..off + 32].try_into().unwrap());
                        let produced = kept.saturating_add(dropped);
                        new_payload[off + 8..off + 16].copy_from_slice(&produced.to_le_bytes());
                        new_payload[off + 16..off + 24].copy_from_slice(&kept.to_le_bytes());
                    }
                }
                emitted_chunks.push((CHUNK_RETIRED, new_payload));
            }

            CHUNK_TRAILER if chunk.payload.len() >= 8 => {
                let n_final = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
                let mut new_payload = chunk.payload.to_vec();
                for i in 0..n_final {
                    let off = 8 + i * FINAL_ENTRY_LEN;
                    if off + FINAL_ENTRY_LEN <= new_payload.len() {
                        let inc = u32::from_le_bytes(new_payload[off..off + 4].try_into().unwrap());
                        let kept = records_kept_per_lane.get(&inc).copied().unwrap_or(0);
                        let dropped =
                            u64::from_le_bytes(new_payload[off + 24..off + 32].try_into().unwrap());
                        let produced = kept.saturating_add(dropped);
                        new_payload[off + 8..off + 16].copy_from_slice(&produced.to_le_bytes());
                        new_payload[off + 16..off + 24].copy_from_slice(&kept.to_le_bytes());
                    }
                }
                emitted_chunks.push((CHUNK_TRAILER, new_payload));
            }

            _ => {
                emitted_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }
        }
    }

    if !export_emitted {
        emitted_chunks.push((CHUNK_EXPORT, export_payload));
    }

    // 8. Re-sequence chunks with chunk_seq 0..N and recompute CRC32.
    let mut out = Vec::with_capacity(bytes.len());
    out.extend_from_slice(&bytes[0..16]);

    for (seq, (ty, payload)) in emitted_chunks.into_iter().enumerate() {
        let mut chunk_head = Vec::with_capacity(16);
        chunk_head.extend_from_slice(&ty.to_le_bytes());
        chunk_head.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        chunk_head.extend_from_slice(&(seq as u64).to_le_bytes());

        let mut full_chunk_bytes = Vec::with_capacity(16 + payload.len());
        full_chunk_bytes.extend_from_slice(&chunk_head);
        full_chunk_bytes.extend_from_slice(&payload);

        let crc = crc32(&full_chunk_bytes);
        full_chunk_bytes.extend_from_slice(&crc.to_le_bytes());

        out.extend_from_slice(&full_chunk_bytes);
    }

    Ok(out)
}

/// Export a frame or frame range from an input file to an output file.
pub fn export_capture<P: AsRef<Path>, Q: AsRef<Path>>(
    in_path: P,
    out_path: Q,
    range: RangeInclusive<u64>,
) -> Result<(), ExportError> {
    let in_bytes = std::fs::read(in_path.as_ref()).map_err(ExportError::Io)?;
    let out_bytes = export_bytes(&in_bytes, range)?;
    std::fs::write(out_path.as_ref(), out_bytes).map_err(ExportError::Io)?;
    Ok(())
}

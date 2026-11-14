//! Standalone capture frame export: one frame or a frame range saved as an independent .prof capture.

use std::collections::BTreeSet;
use std::ops::RangeInclusive;
use std::path::Path;

use crate::auto_rsx::{
    CHUNK_AUTO_CONFIG, CHUNK_AUTO_COVERAGE, CHUNK_AUTO_OPS, CHUNK_AUTO_STATUS,
    MASKS_BUILD, OP_BYTES, OP_BYTES_BUILD, OP_BUILD_KNOWN, OP_TOKEN, ST_LOWER_BOUND,
};
use crate::fifo::{
    CHUNK_FIFO_EVENTS, CHUNK_FIFO_STATUS, EV_TOKEN_PRESENT, KIND_WRAP_ENTER,
    KIND_WRAP_EXIT, ST_SATURATED,
};
use crate::hash::crc32;
use crate::model::*;

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

    struct RawChunk<'a> {
        ty: u32,
        payload: &'a [u8],
    }

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

    let mut present_frames = BTreeSet::new();
    for chunk in &raw_chunks {
        if chunk.ty == CHUNK_AUTO_COVERAGE && chunk.payload.len() >= 24 {
            let frame = u64::from_le_bytes(chunk.payload[16..24].try_into().unwrap());
            present_frames.insert(frame);
        } else if chunk.ty == CHUNK_RECORDS && chunk.payload.len() >= 8 {
            let n = u32::from_le_bytes(chunk.payload[4..8].try_into().unwrap()) as usize;
            for i in 0..n {
                let off = 8 + i * 32;
                if off + 32 <= chunk.payload.len() {
                    let kind = chunk.payload[off];
                    if kind == 4 {
                        let frame_id =
                            u64::from_le_bytes(chunk.payload[off + 24..off + 32].try_into().unwrap());
                        present_frames.insert(frame_id);
                    }
                }
            }
        }
    }

    for f in range.clone() {
        if !present_frames.contains(&f) {
            return Err(ExportError::FrameNotFound(f));
        }
    }

    let mut op_width = OP_BYTES as usize;
    for chunk in &raw_chunks {
        if chunk.ty == CHUNK_AUTO_CONFIG && chunk.payload.len() >= 24 {
            let kinds = u32::from_le_bytes(chunk.payload[16..20].try_into().unwrap());
            let forms = u32::from_le_bytes(chunk.payload[20..24].try_into().unwrap());
            if (kinds, forms) == MASKS_BUILD || (kinds, forms) == (127, 11) {
                op_width = OP_BYTES_BUILD as usize;
            }
            break;
        }
    }

    let last_auto_status_idx = raw_chunks
        .iter()
        .rposition(|c| c.ty == CHUNK_AUTO_STATUS);
    let last_fifo_status_idx = raw_chunks
        .iter()
        .rposition(|c| c.ty == CHUNK_FIFO_STATUS);

    let mut out_chunks: Vec<(u32, Vec<u8>)> = Vec::new();
    let mut exported_frames_count = 0u64;
    let mut exported_ops_count = 0u64;
    let mut exported_fifo_events: Vec<Vec<u8>> = Vec::new();

    let mut last_auto_status_payload: Option<Vec<u8>> = None;
    let mut last_fifo_status_payload: Option<Vec<u8>> = None;
    let mut trailer_chunk: Option<Vec<u8>> = None;

    for (idx, chunk) in raw_chunks.into_iter().enumerate() {
        match chunk.ty {
            CHUNK_AUTO_COVERAGE => {
                if chunk.payload.len() >= 24 {
                    let frame = u64::from_le_bytes(chunk.payload[16..24].try_into().unwrap());
                    if range.contains(&frame) {
                        out_chunks.push((chunk.ty, chunk.payload.to_vec()));
                        exported_frames_count += 1;
                    }
                }
            }
            CHUNK_AUTO_OPS => {
                if chunk.payload.len() >= 16 {
                    let count =
                        u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                    let width =
                        u32::from_le_bytes(chunk.payload[12..16].try_into().unwrap()) as usize;
                    let stride = if width > 0 { width } else { op_width };
                    let mut matching = Vec::new();
                    for i in 0..count {
                        if 16 + (i + 1) * stride > chunk.payload.len() {
                            break;
                        }
                        let rec = &chunk.payload[16 + i * stride..16 + (i + 1) * stride];
                        let flags = u16::from_le_bytes(rec[14..16].try_into().unwrap());
                        let frame = u64::from_le_bytes(rec[16..24].try_into().unwrap());
                        let token_match = (flags & OP_TOKEN != 0) && range.contains(&frame);
                        let mut build_match = false;
                        if stride == OP_BYTES_BUILD as usize && rec.len() >= 144 {
                            let b_frame =
                                u64::from_le_bytes(rec[128..136].try_into().unwrap());
                            let b_flags =
                                u16::from_le_bytes(rec[140..142].try_into().unwrap());
                            build_match =
                                (b_flags & OP_BUILD_KNOWN != 0) && range.contains(&b_frame);
                        }
                        if token_match || build_match {
                            matching.extend_from_slice(rec);
                            exported_ops_count += 1;
                        }
                    }
                    if !matching.is_empty() {
                        let mut new_payload = Vec::with_capacity(16 + matching.len());
                        new_payload.extend_from_slice(&chunk.payload[0..8]);
                        let rec_count = (matching.len() / stride) as u32;
                        new_payload.extend_from_slice(&rec_count.to_le_bytes());
                        new_payload.extend_from_slice(&(stride as u32).to_le_bytes());
                        new_payload.extend_from_slice(&matching);
                        out_chunks.push((chunk.ty, new_payload));
                    }
                }
            }
            CHUNK_FIFO_EVENTS => {
                if chunk.payload.len() >= 16 {
                    let count =
                        u32::from_le_bytes(chunk.payload[8..12].try_into().unwrap()) as usize;
                    let mut matching = Vec::new();
                    for i in 0..count {
                        if 16 + (i + 1) * 96 > chunk.payload.len() {
                            break;
                        }
                        let rec = &chunk.payload[16 + i * 96..16 + (i + 1) * 96];
                        let flags = u16::from_le_bytes(rec[38..40].try_into().unwrap());
                        if flags & EV_TOKEN_PRESENT != 0 {
                            let frame_id =
                                u64::from_le_bytes(rec[48..56].try_into().unwrap());
                            if range.contains(&frame_id) {
                                matching.extend_from_slice(rec);
                                exported_fifo_events.push(rec.to_vec());
                            }
                        }
                    }
                    if !matching.is_empty() {
                        let mut new_payload = Vec::with_capacity(16 + matching.len());
                        new_payload.extend_from_slice(&chunk.payload[0..8]);
                        let rec_count = (matching.len() / 96) as u32;
                        new_payload.extend_from_slice(&rec_count.to_le_bytes());
                        new_payload.extend_from_slice(&chunk.payload[12..16]);
                        new_payload.extend_from_slice(&matching);
                        out_chunks.push((chunk.ty, new_payload));
                    }
                }
            }
            CHUNK_AUTO_STATUS => {
                if Some(idx) == last_auto_status_idx {
                    last_auto_status_payload = Some(chunk.payload.to_vec());
                }
            }
            CHUNK_FIFO_STATUS => {
                if Some(idx) == last_fifo_status_idx {
                    last_fifo_status_payload = Some(chunk.payload.to_vec());
                }
            }
            CHUNK_TRAILER => {
                trailer_chunk = Some(chunk.payload.to_vec());
            }
            _ => {
                out_chunks.push((chunk.ty, chunk.payload.to_vec()));
            }
        }
    }

    if let Some(mut status_payload) = last_auto_status_payload {
        if status_payload.len() >= 128 {
            let flags = u16::from_le_bytes(status_payload[2..4].try_into().unwrap());
            let mut counters = [0u64; 15];
            for (i, c) in counters.iter_mut().enumerate() {
                *c = u64::from_le_bytes(
                    status_payload[8 + i * 8..16 + i * 8].try_into().unwrap(),
                );
            }
            counters[2] = exported_ops_count;
            counters[5] = exported_frames_count;
            if flags & ST_LOWER_BOUND == 0 {
                counters[1] = exported_ops_count + counters[3] + counters[4];
            } else {
                counters[1] = counters[1].max(exported_ops_count + counters[3] + counters[4]);
            }
            for (i, c) in counters.iter().enumerate() {
                status_payload[8 + i * 8..16 + i * 8].copy_from_slice(&c.to_le_bytes());
            }
        }
        out_chunks.push((CHUNK_AUTO_STATUS, status_payload));
    }

    if let Some(mut fifo_status_payload) = last_fifo_status_payload {
        if fifo_status_payload.len() >= 80 {
            let flags = u16::from_le_bytes(fifo_status_payload[2..4].try_into().unwrap());
            let original_attempted =
                u64::from_le_bytes(fifo_status_payload[8..16].try_into().unwrap());
            let original_dropped =
                u64::from_le_bytes(fifo_status_payload[24..32].try_into().unwrap());
            let written = exported_fifo_events.len() as u64;
            let last_seq = exported_fifo_events.last().map_or(0, |rec| {
                u64::from_le_bytes(rec[0..8].try_into().unwrap())
            });
            let enter_count = exported_fifo_events
                .iter()
                .filter(|rec| {
                    u16::from_le_bytes(rec[36..38].try_into().unwrap()) == KIND_WRAP_ENTER
                })
                .count() as u64;
            let exit_count = exported_fifo_events
                .iter()
                .filter(|rec| {
                    u16::from_le_bytes(rec[36..38].try_into().unwrap()) == KIND_WRAP_EXIT
                })
                .count() as u64;

            let attempted = original_attempted
                .max(written + original_dropped)
                .max(last_seq);
            fifo_status_payload[8..16].copy_from_slice(&attempted.to_le_bytes());
            fifo_status_payload[16..24].copy_from_slice(&written.to_le_bytes());
            fifo_status_payload[40..48].copy_from_slice(&last_seq.to_le_bytes());
            if flags & ST_SATURATED == 0 {
                fifo_status_payload[48..56].copy_from_slice(&enter_count.to_le_bytes());
                fifo_status_payload[56..64].copy_from_slice(&exit_count.to_le_bytes());
            }
        }
        out_chunks.push((CHUNK_FIFO_STATUS, fifo_status_payload));
    }

    if let Some(trailer) = trailer_chunk {
        out_chunks.push((CHUNK_TRAILER, trailer));
    }

    let mut out = Vec::with_capacity(bytes.len());
    out.extend_from_slice(&bytes[0..16]);

    for (seq, (ty, payload)) in out_chunks.into_iter().enumerate() {
        let mut full_chunk_bytes = Vec::with_capacity(16 + payload.len() + 4);
        full_chunk_bytes.extend_from_slice(&ty.to_le_bytes());
        full_chunk_bytes.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        full_chunk_bytes.extend_from_slice(&(seq as u64).to_le_bytes());
        full_chunk_bytes.extend_from_slice(&payload);
        let crc = crc32(&full_chunk_bytes);
        full_chunk_bytes.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&full_chunk_bytes);
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

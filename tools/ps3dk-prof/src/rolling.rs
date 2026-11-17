//! Offline PPU/WINDOWS restart slice. No physical-container, runtime, live, SPU,
//! AUTO or FIFO support is implied. Results deliberately have their own type;
//! they are not a legacy full-capture Analysis with hidden validation exemptions.
use std::collections::{BTreeMap, BTreeSet};
use crate::hash::crc32;
use crate::model::*;
use crate::reader::PpuState;
use crate::rolling_checkpoint as checkpoint;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub metadata_bytes: usize,
    pub incarnations: usize,
    pub descriptors: usize,
    /// Combined frame-membership runs and unresolved prefix token identities.
    pub frame_runs: usize,
    pub stack_rows: usize,
    /// Input RECORDS rows per offline operation, including GAPs.
    pub work_records: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { checkpoint_bytes: 8 << 20, metadata_bytes: 4 << 20, incarnations: 4096,
            descriptors: 4096, frame_runs: 65536, stack_rows: 262144, work_records: 1_000_000 }
    }
}
#[derive(Debug)]
pub enum Error {
    Unsupported { flags: u32, chunk: Option<u32> },
    Limit(&'static str),
    Checkpoint(&'static str),
    Malformed(Malformed),
}
impl From<Malformed> for Error { fn from(value: Malformed) -> Self { Self::Malformed(value) } }
#[derive(Debug)]
pub struct LaneSummary {
    pub prefix_written: u64,
    pub retained_written: u64,
    pub prefix_gap_sum: u64,
    pub retained_gap_sum: u64,
    pub final_entry: Option<FinalEntry>,
}
#[derive(Debug)]
pub struct Zone {
    pub name: String,
    pub completed: u64,
    pub left_censored: u64,
    /// Open or producer stop-censored retained instances; can overlap left_censored.
    pub open: u64,
    /// Only complete retained instances on valid lanes. None when no such
    /// instances exist or an included lane/duration cannot support a measurement.
    pub total_ticks: Option<u128>,
}
#[derive(Debug)]
pub struct Window {
    pub original: CaptureWindow,
    pub left_clipped: bool,
}
#[derive(Debug)]
pub struct RetainedPpu {
    pub lanes: BTreeMap<u32, LaneSummary>,
    pub zones: Vec<Zone>,
    pub windows: Vec<Window>,
    pub incomplete: Vec<Incomplete>,
    pub first_chunk: u64,
    pub next_chunk: u64,
    /// Original chunk bytes preceding this window (not producer loss).
    pub prefix_bytes: u64,
}

pub(crate) struct Chunk<'a> {
    pub index: u64,
    pub ty: u32,
    pub payload: &'a [u8],
    pub raw: &'a [u8],
}
fn bad(rule: Rule, detail: &str) -> Error { Error::Malformed(Malformed { rule, detail: detail.into() }) }
pub(crate) fn supported(flags: u32) -> Result<(), Error> {
    if flags & !FLAG_WINDOWS != 0 { return Err(Error::Unsupported { flags, chunk: None }); }
    Ok(())
}
pub(crate) fn scan(bytes: &[u8], first: u64, flags: u32, limits: Limits)
    -> Result<(Vec<Chunk<'_>>, bool, u64), Error> {
    let mut offset = 0usize;
    let mut index = first;
    let mut rows = 0u64;
    let mut metadata = 16usize;
    let mut trailer = false;
    let mut out = Vec::new();
    while offset < bytes.len() {
        let b = &bytes[offset..];
        if trailer {
            return Err(bad(if b.len() >= 16 && u32::from_le_bytes(b[..4].try_into().unwrap()) == CHUNK_TRAILER {
                Rule::SecondTrailer
            } else { Rule::TrailingBytesAfterTrailer }, "bytes after TRAILER"));
        }
        if b.len() < 16 { return Ok((out, true, index)); }
        let ty = u32::from_le_bytes(b[..4].try_into().unwrap());
        let len = u32::from_le_bytes(b[4..8].try_into().unwrap());
        if len > MAX_CHUNK_LEN { return Err(bad(Rule::ChunkLenOverCap, "rolling chunk length")); }
        let total = len as usize + 20;
        if b.len() < total { return Ok((out, true, index)); }
        if crc32(&b[..total - 4]) != u32::from_le_bytes(b[total - 4..total].try_into().unwrap()) {
            return Err(bad(Rule::ChunkCrc, "rolling original chunk CRC"));
        }
        if u64::from_le_bytes(b[8..16].try_into().unwrap()) != index {
            return Err(bad(Rule::ChunkSeqOrder, "rolling original chunk sequence"));
        }
        if !(1..=7).contains(&ty) && !(flags & FLAG_WINDOWS != 0 && matches!(ty, CHUNK_WINDOW_BEGIN | CHUNK_WINDOW_END)) {
            return Err(Error::Unsupported { flags, chunk: Some(ty) });
        }
        let payload = &b[16..total - 4];
        if ty == CHUNK_RECORDS {
            if payload.len() < 8 { return Err(bad(Rule::RecordsLength, "short RECORDS")); }
            let n = u32::from_le_bytes(payload[4..8].try_into().unwrap());
            // Validate fixed shape before collecting chunks. Otherwise n=0
            // could evade the work budget while growing the chunk inventory.
            if n == 0 { return Err(bad(Rule::RecordsCountZero, "empty RECORDS")); }
            if payload.len() as u128 != 8 + 32 * u128::from(n) {
                return Err(bad(Rule::RecordsLength, "RECORDS row count/length"));
            }
            rows = rows.checked_add(u64::from(n)).ok_or(Error::Limit("record work"))?;
            if rows > limits.work_records { return Err(Error::Limit("record work")); }
        } else {
            metadata = metadata.checked_add(total).ok_or(Error::Limit("metadata bytes"))?;
            if metadata > limits.metadata_bytes { return Err(Error::Limit("metadata bytes")); }
        }
        out.try_reserve(1).map_err(|_| Error::Limit("chunk allocation"))?;
        out.push(Chunk { index, ty, payload, raw: &b[..total] });
        trailer = ty == CHUNK_TRAILER;
        index = index.checked_add(1).ok_or(Error::Limit("chunk sequence"))?;
        offset += total;
    }
    Ok((out, false, index))
}
fn state_limits(s: &PpuState, limits: Limits) -> Result<(), Error> {
    if s.capture().lanes.len() > limits.incarnations { return Err(Error::Limit("incarnations")); }
    if s.capture().descriptors.len() > limits.descriptors { return Err(Error::Limit("descriptors")); }
    let stacks = s.capture().lanes.values().try_fold(0usize, |n, l| n.checked_add(l.stack.len()))
        .ok_or(Error::Limit("stack rows"))?;
    if stacks > limits.stack_rows { return Err(Error::Limit("stack rows")); }
    Ok(())
}

/// Create a bounded semantic checkpoint at an exact original chunk boundary.
/// The prefix may end before HEADER/CLOCKS, but cannot be torn or contain a
/// TRAILER. Historical closed event rows are compacted, never replayed.
pub fn checkpoint_ppu(prefix: &[u8], limits: Limits) -> Result<Vec<u8>, Error> {
    let header: [u8; 16] = prefix.get(..16).ok_or(Error::Checkpoint("short original file header"))?
        .try_into().unwrap();
    let mut state = PpuState::new(&header)?;
    let flags = u32::from_le_bytes(header[12..].try_into().unwrap());
    supported(flags)?;
    let (chunks, torn, next) = scan(&prefix[16..], 0, flags, limits)?;
    if torn { return Err(Error::Checkpoint("prefix ends inside a chunk")); }
    if chunks.iter().any(|c| c.ty == CHUNK_TRAILER) { return Err(Error::Checkpoint("checkpoint after TRAILER")); }
    checkpoint::check_metadata_budget(None, &chunks, limits)?;
    for c in &chunks { state.accept(c.index, c.ty, c.payload)?; state_limits(&state, limits)?; }
    let cp = checkpoint::snapshot(&state, header, &chunks, next, (prefix.len() - 16) as u64, limits)?;
    // Retired entries already present must conserve even though there is no
    // TRAILER yet. Snapshot before finish, which intentionally clears stacks.
    state.finish(false)?;
    checkpoint::encode(&cp, limits)
}

/// Advance a checkpoint over whole original chunks, without reconstructing
/// evicted event history. The result is canonical: compare its bytes against
/// the next segment's checkpoint to verify that segment-chain transition.
/// An empty payload is an identity. A TRAILER is terminal and has no successor
/// checkpoint; torn payloads cannot establish a checkpoint boundary.
pub fn advance_ppu(bytes: &[u8], payload: &[u8], limits: Limits) -> Result<Vec<u8>, Error> {
    let previous = checkpoint::decode(bytes, limits)?;
    let flags = u32::from_le_bytes(previous.header[12..].try_into().unwrap());
    let (chunks, torn, next) = scan(payload, previous.next_chunk, flags, limits)?;
    if torn { return Err(Error::Checkpoint("advance ends inside a chunk")); }
    if chunks.iter().any(|c| c.ty == CHUNK_TRAILER) { return Err(Error::Checkpoint("TRAILER has no successor checkpoint")); }
    checkpoint::check_metadata_budget(Some(&previous), &chunks, limits)?;
    let mut state = checkpoint::restore(&previous, limits)?;
    for c in &chunks { state.accept(c.index, c.ty, c.payload)?; state_limits(&state, limits)?; }
    let prefix_bytes = previous.prefix_bytes.checked_add(payload.len() as u64)
        .ok_or(Error::Checkpoint("prefix byte frontier overflow"))?;
    let cp = checkpoint::snapshot_after(&state, previous.header, &chunks, next, prefix_bytes, limits, Some(&previous))?;
    // Includes exact prefix + retained accounting for any final entries; no
    // event rows are recreated to make these checks pass.
    state.finish(false)?;
    checkpoint::encode(&cp, limits)
}

/// Import the typed checkpoint and read original framed suffix chunks. This
/// does not construct a v1 byte stream, synthesize GAPs, or use EXPORT metadata.
pub fn read_ppu_suffix(bytes: &[u8], suffix: &[u8], limits: Limits) -> Result<RetainedPpu, Error> {
    let cp = checkpoint::decode(bytes, limits)?;
    let flags = u32::from_le_bytes(cp.header[12..].try_into().unwrap());
    let (chunks, torn, next) = scan(suffix, cp.next_chunk, flags, limits)?;
    checkpoint::check_metadata_budget(Some(&cp), &chunks, limits)?;
    let mut state = checkpoint::restore(&cp, limits)?;
    let (closed, open) = state.window_boundary();
    for c in &chunks { state.accept(c.index, c.ty, c.payload)?; state_limits(&state, limits)?; }
    let cap = state.finish(torn)?;
    // Token rule 4 still applies, including unresolved references from closed
    // prefix spans that were compacted away. Recorded prefix frames are proved
    // by exact membership runs, not by guessing every ID below a high-water mark.
    let tokens = cp.tokens.iter().copied().chain(cap.spans.iter().filter_map(|s| s.token));
    for (owner, frame) in tokens {
        let lane = cap.lanes.get(&owner).ok_or_else(|| bad(Rule::UndeclaredTokenOwner, "checkpoint token owner"))?;
        if lane.frames.iter().any(|f| f.id == frame)
            || cp.rows.get(&owner).is_some_and(|r| r.frames.iter().any(|&(a, b)| a <= frame && frame <= b)) { continue; }
        if !cap.has_trailer() || cap.torn() || lane.final_entry.is_none() { continue; }
        let final_ = lane.final_entry.as_ref().unwrap();
        let prefix_loss = cp.rows.get(&owner).is_some_and(|r| r.gap_count != 0);
        if prefix_loss || lane.gap_count > 0 || final_.state == STATE_NOT_QUIESCED || final_.dropped_count > 0 { continue; }
        return Err(bad(Rule::TokenImpossible, "token names a frame never recorded by a lossless quiesced owner"));
    }
    let left: BTreeSet<(u32, u64)> = cp.rows.iter().flat_map(|(&inc, r)| r.stack.iter().map(move |s| (inc, s.begin.c))).collect();
    let mut lanes = BTreeMap::new();
    for (&inc, l) in &cap.lanes {
        let prefix = cp.rows.get(&inc);
        lanes.insert(inc, LaneSummary { prefix_written: prefix.map_or(0, |p| p.written), retained_written: l.written,
            prefix_gap_sum: prefix.map_or(0, |p| p.gaps), retained_gap_sum: l.gap_sum as u64,
            final_entry: l.final_entry.clone() });
    }
    let mut zones: BTreeMap<String, (Zone, bool)> = BTreeMap::new();
    for s in &cap.spans {
        let name = cap.descriptors[&s.desc].name.clone();
        let (z, invalid) = zones.entry(name.clone()).or_insert((Zone {
            name, completed: 0, left_censored: 0, open: 0, total_ticks: None,
        }, false));
        if s.is_censored() { z.open += 1; }
        if left.contains(&(s.incarnation, s.span_id)) { z.left_censored += 1; continue; }
        if s.is_censored() { continue; }
        z.completed += 1;
        let lane = &cap.lanes[&s.incarnation];
        let valid = cap.incomplete.is_empty() && !lane.clock_anomaly && !s.affected
            && lane.final_entry.as_ref().is_some_and(|e| e.state != STATE_NOT_QUIESCED
                && e.dropped_count == 0 && e.refused_count == 0 && e.regerr_count == 0 && e.limit_flags == 0);
        if let Some(ticks) = s.inclusive().filter(|_| valid) {
            z.total_ticks = Some(z.total_ticks.unwrap_or(0) + u128::from(ticks));
        } else { *invalid = true; }
    }
    let zones = zones.into_values().map(|(mut z, invalid)| { if invalid { z.total_ticks = None; } z }).collect();
    let windows = cap.windows.into_iter().filter(|w| !closed.contains(&w.id)).map(|w| {
        let left_clipped = open == Some(w.id) || (!cap.windows_flag && cp.rows.values().any(|r| r.written > 0));
        Window { original: w, left_clipped }
    }).collect();
    Ok(RetainedPpu { lanes, zones, windows, incomplete: cap.incomplete,
        first_chunk: cp.next_chunk, next_chunk: next, prefix_bytes: cp.prefix_bytes })
}

//! Offline physical-container joins for PPU/WINDOWS and combined PPU/SPU.
//! No runtime, live, CLI, GPU/AUTO/FIFO or full-family claim.
//!
//! The caller supplies a coherent immutable container snapshot. Physical
//! recovery selects one complete root chain; a semantic mismatch in that chain
//! is an error, not permission to choose an older root or omit a segment.
//!
//! The PPU-only join uses one temporary concatenation bounded by the physical
//! caller cap. The combined join borrows payloads and checkpoints, adding at
//! most 64 descriptors to the physical reader's at most 128 descriptors. It
//! keeps one semantic state and one canonical boundary buffer plus bounded
//! encoder temporaries. These are admission bounds, not exact RSS promises.
use crate::{rolling, rolling_combined, rolling_store};

/// Desktop CLI/viewer allowance for conservative combined-analysis storage.
/// Separate from file admission and from the caller's retained-output budget;
/// not a process RSS bound. Library Limits::default remains 64 MiB. Six default
/// 1 MiB payload slots can retain enough AUTO rows to exceed that smaller cap.
pub const DEFAULT_ANALYSIS_BYTES: usize = 256 << 20;
/// Conservative analysis work units, including the AUTO cross-product charge;
/// units are not CPU instructions or a wall-clock deadline.
pub const DEFAULT_ANALYSIS_WORK: u64 = 1 << 38;

/// Explicit desktop policy, without changing conservative library defaults.
pub fn desktop_limits() -> rolling_combined::Limits {
    rolling_combined::Limits { temporary_bytes: DEFAULT_ANALYSIS_BYTES,
        work_items: DEFAULT_ANALYSIS_WORK, ..Default::default() }
}

#[derive(Debug)]
pub enum Error {
    Store(rolling_store::Error),
    Semantic(rolling::Error),
    EmptyStore,
    Boundary(&'static str),
    Transition { segment: u64 },
    Limit(&'static str),
}
#[derive(Debug)]
pub struct PpuFile { pub store_view: rolling_store::View, pub retained: rolling::RetainedPpu }

pub struct CombinedFile {
    /// Physical OPEN/SEALED/STOPPED_RESOURCE is separate from source finals.
    pub store_view: rolling_store::View,
    /// All retained slots share one report; SPU placement is causal support only.
    pub retained: crate::rolling_combined::RetainedCombined,
}
#[derive(Debug)]
pub enum CombinedError {
    Store(rolling_store::Error),
    Semantic(crate::rolling_combined::Error),
    EmptyStore,
    Boundary { segment: Option<u64>, field: &'static str },
    Transition { segment: u64 },
    Limit(&'static str),
}

impl std::fmt::Display for CombinedError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Store(error) => write!(f, "rolling physical validation: {error}"),
            Self::Semantic(crate::rolling_combined::Error::Limit(reason)) =>
                write!(f, "rolling combined analysis limit: {reason}"),
            Self::Semantic(error) => write!(f, "rolling combined validation: {error:?}"),
            Self::EmptyStore => f.write_str("empty rolling store has no semantic checkpoint"),
            Self::Boundary { segment: Some(segment), field } =>
                write!(f, "rolling checkpoint boundary mismatch at segment {segment}: {field}"),
            Self::Boundary { segment: None, field } =>
                write!(f, "rolling checkpoint boundary mismatch: {field}"),
            Self::Transition { segment } =>
                write!(f, "checkpoint does not follow preceding payload at segment {segment}"),
            Self::Limit(reason) => write!(f, "rolling join admission limit: {reason}"),
        }
    }
}
impl std::error::Error for CombinedError {}

/// Read one coherent immutable snapshot with one physical root selection.
/// Every adjacent checkpoint must equal the canonical state after the previous
/// payload. Semantic rejection never selects an older root. The earliest
/// checkpoint remains an unauthenticated claim about the evicted prefix.
///
/// Input bytes, events, metadata, structural work and position reservations are
/// cumulative across the selected chain. Defaults reserve 2^24 position work
/// with 2^20 per proof, so a physically valid 64-slot chain may need larger
/// explicit semantic limits. Empty segments still require boundary proofs.
/// No runtime/live/full-family feature or tick/RMAX1 qualification is implied;
/// SPU counter and lift history remain explicitly unevaluated.
pub fn read_combined(bytes: &[u8], store_limits: rolling_store::Limits,
    limits: rolling_combined::Limits,
    options: rolling_combined::ReportOptions<'_>) -> Result<CombinedFile, CombinedError> {
    let store = rolling_store::read(bytes, store_limits).map_err(CombinedError::Store)?;
    if store.segments.is_empty() { return Err(CombinedError::EmptyStore); }
    if store.segments.len() > 64 { return Err(CombinedError::Limit("retained segments")); }
    let mut segments = Vec::new();
    segments.try_reserve_exact(store.segments.len()).map_err(|_| CombinedError::Limit("segment allocation"))?;
    for s in &store.segments {
        segments.push(rolling_combined::SegmentInput { segment: s.segment,
            first_chunk: s.first_chunk, next_chunk: s.next_chunk, checkpoint: s.checkpoint, payload: s.payload });
    }
    let bounds = rolling_combined::ChainBounds { first_chunk: store.view.first_chunk,
        next_chunk: store.view.next_chunk, evicted_payload_bytes: store.view.evicted_payload_bytes,
        bound_payload_bytes: (store.view.format_version == 2).then_some(store.view.bound_payload_bytes) };
    let retained = rolling_combined::read_segmented(&segments, bounds, limits, options).map_err(|e| match e {
        rolling_combined::ChainError::Semantic(e) => CombinedError::Semantic(e),
        rolling_combined::ChainError::Boundary { segment, field } => CombinedError::Boundary { segment, field },
        rolling_combined::ChainError::Transition { segment } => CombinedError::Transition { segment },
        rolling_combined::ChainError::Limit(s) => CombinedError::Limit(s),
    })?;
    Ok(CombinedFile { store_view: store.view, retained })
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Store(error) => write!(f, "rolling physical validation: {error}"),
            Self::Semantic(error) => write!(f, "rolling PPU validation: {error:?}"),
            Self::EmptyStore => f.write_str("empty rolling store has no semantic checkpoint"),
            Self::Boundary(reason) => write!(f, "rolling physical/PPU boundary mismatch: {reason}"),
            Self::Transition { segment } => write!(f, "checkpoint does not follow preceding payload at segment {segment}"),
            Self::Limit(reason) => write!(f, "rolling join admission limit: {reason}"),
        }
    }
}
impl std::error::Error for Error {}

/// Analyze all retained PPU/WINDOWS payloads from the first retained checkpoint.
/// Evicted prefix state remains a checkpoint claim; this does not reconstruct
/// or authenticate evicted events. Every retained checkpoint transition must
/// agree exactly, and no partial result escapes on any mismatch.
pub fn read_ppu(bytes: &[u8], store_limits: rolling_store::Limits, ppu_limits: rolling::Limits)
    -> Result<PpuFile, Error> {
    let store = rolling_store::read(bytes, store_limits).map_err(Error::Store)?;
    let first = store.segments.first().ok_or(Error::EmptyStore)?;
    let total = store.segments.iter().try_fold(0usize, |n, segment| n.checked_add(segment.payload.len()))
        .ok_or(Error::Limit("retained payload byte sum"))?;
    if u64::try_from(total).map_err(|_| Error::Limit("retained payload byte sum"))? > store_limits.max_file_bytes {
        return Err(Error::Limit("retained payload bytes"));
    }
    let mut payload = Vec::new();
    payload.try_reserve_exact(total).map_err(|_| Error::Limit("retained payload allocation"))?;
    for segment in &store.segments { payload.extend_from_slice(segment.payload); }

    // One suffix operation charges work_records across ALL retained segments
    // and keeps every retained report. Reading only the last segment would
    // silently discard earlier retained spans and reset the work budget.
    let retained = rolling::read_ppu_suffix(first.checkpoint, &payload, ppu_limits).map_err(Error::Semantic)?;
    drop(payload);
    if retained.first_chunk != store.view.first_chunk {
        return Err(Error::Boundary("first chunk"));
    }
    if retained.prefix_bytes != store.view.evicted_payload_bytes {
        return Err(Error::Boundary("evicted prefix bytes"));
    }
    if retained.next_chunk != store.view.next_chunk {
        return Err(Error::Boundary("next chunk"));
    }

    // The first validated checkpoint is anchored above. Physical validation
    // proves contiguous chunk ranges; exact canonical advancement then anchors
    // each next checkpoint's boundary/state by induction, including empty
    // payloads. No duplicated checkpoint wire parser is needed here.
    for pair in store.segments.windows(2) {
        let next = rolling::advance_ppu(pair[0].checkpoint, pair[0].payload, ppu_limits).map_err(Error::Semantic)?;
        if next.as_slice() != pair[1].checkpoint {
            return Err(Error::Transition { segment: pair[1].segment });
        }
    }
    // Do not advance the last segment: its terminal TRAILER is allowed. A
    // TRAILER in any earlier segment is refused by advance_ppu, even when a
    // subsequent segment has no payload bytes for suffix scanning to notice.
    Ok(PpuFile { store_view: store.view, retained })
}

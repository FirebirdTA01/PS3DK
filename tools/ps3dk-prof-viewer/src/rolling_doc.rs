//! A bounded rolling capture (`.profr`): the retained window only.
//!
//! A rolling file keeps the newest committed segments of a capture and drops
//! the oldest. The reader validates the retained segments and projects what
//! they hold per family; it builds no whole-capture `Analysis`, and this
//! module never fabricates one. Every number here covers the retained part
//! only: the dropped prefix is a checkpoint claim, not authenticated history.
//!
//! Detection is by content (the container magic), never only by extension.
//! The open is bounded: the file must fit the file admission cap before it
//! is read, and the library's shared desktop limits (analysis storage and
//! work) bound the reader's analysis.

use ps3dk_prof::{model::Incomplete, rolling, rolling_combined as mixed, rolling_file as join, rolling_store as store};
use std::io::Read;
use std::path::{Path, PathBuf};

/// The container magic of a rolling store (`PS3DKROL`).
pub const MAGIC: &[u8; 8] = b"PS3DKROL";

/// File admission cap for a saved rolling file, matching the CLI's default
/// `--max-bytes` (64 MiB). Separate from the analysis storage allowance.
pub const FILE_BYTES: u64 = 64 << 20;

/// The explicit bounds of one rolling open: file admission plus the combined
/// reader's limits. The viewer uses the library's shared desktop policy
/// ([`join::desktop_limits`]: `DEFAULT_ANALYSIS_BYTES` storage and
/// `DEFAULT_ANALYSIS_WORK` work units), never the conservative raw defaults.
#[derive(Clone, Copy, Debug)]
pub struct OpenLimits {
    pub file_bytes: u64,
    pub analysis: mixed::Limits,
}
impl OpenLimits {
    /// The desktop policy, exactly as `ps3dk-prof rolling` applies it by default.
    pub fn desktop() -> OpenLimits {
        let file_bytes = FILE_BYTES;
        OpenLimits {
            file_bytes,
            analysis: mixed::Limits { input_bytes: file_bytes as usize, ..join::desktop_limits() },
        }
    }
}

/// True when these leading bytes are a rolling store container.
pub fn is_rolling(head: &[u8]) -> bool {
    head.len() >= MAGIC.len() && &head[..MAGIC.len()] == MAGIC
}

/// Reads at most the first eight bytes of `path` and reports whether it is a rolling store.
pub fn sniff(path: &Path) -> std::io::Result<bool> {
    let mut f = std::fs::File::open(path)?;
    let mut head = [0u8; 8];
    let mut n = 0;
    while n < head.len() {
        match f.read(&mut head[n..])? {
            0 => break,
            k => n += k,
        }
    }
    Ok(is_rolling(&head[..n]))
}

/// Physical store facts (the selected committed root chain).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Physical {
    pub version: u32,
    /// OPEN, SEALED, STOPPED_RESOURCE, or UNSPECIFIED (v1 has no state).
    pub state: &'static str,
    pub reason: u32,
    pub recovered: bool,
    pub generation: u64,
    /// v2 only: evicted plus retained payload bytes bound by the root.
    pub bound_payload_bytes: Option<u64>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RetainedRange {
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub segments: u32,
    pub payload_bytes: u64,
    /// Original payload offset of the first retained byte (= evicted payload bytes).
    pub first_payload_byte: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Evicted {
    pub chunks: u64,
    pub payload_bytes: u64,
    pub segments: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ZoneRow {
    pub name: String,
    pub completed: u64,
    pub left_censored: u64,
    pub open: u64,
    pub total_ticks: Option<u128>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LaneRow {
    pub id: u32,
    pub prefix_written: u64,
    pub retained_written: u64,
    pub retained_gap_sum: u64,
    pub final_present: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Ppu {
    pub zones: Vec<ZoneRow>,
    pub lanes: Vec<LaneRow>,
    pub incomplete: Vec<String>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SpuSource {
    pub source: String,
    pub retained_events: u64,
    pub prefix_events: u64,
    pub retained_lost: u64,
    pub pairs: usize,
    pub left_censored: usize,
    pub open_retained: usize,
    pub final_state: Option<u8>,
    pub raw_valid: bool,
    pub structure: String,
    pub protocol_error: Option<String>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Spu {
    pub note: String,
    pub refusal: Option<String>,
    pub sources: Vec<SpuSource>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AutoFrame {
    pub owner: u32,
    pub frame: u64,
    /// The verdict's key (as the CLI prints it) and its UI label.
    pub verdict: String,
    pub verdict_label: &'static str,
    pub reason: String,
    pub rsx_busy_raw: Option<u64>,
    pub rsx_busy_ppu: Option<(u64, u64)>,
    pub unavailable: Vec<String>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Auto {
    /// The clock map's full debug form (as the CLI prints it).
    pub map: String,
    /// Qualified, or the exact missing condition.
    pub map_line: String,
    pub frames: Vec<AutoFrame>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Fifo {
    pub final_seen: bool,
    pub retained_events: u64,
    pub retained_accounting_rows: u64,
    pub unavailable_window_rows:u64,
    pub retained_unavailable_window_rows:usize,
    pub known_missing: u64,
    pub prefix_unassociated: bool,
    pub boundary_frames: Vec<(u32, u64, u64)>,
}
/// Source schema: PPU/WINDOWS only, or combined PPU/SPU (+ AUTO/FIFO).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Schema {
    Ppu,
    Combined,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Summary {
    pub schema: Schema,
    pub file_bytes: u64,
    pub physical: Physical,
    pub retained: RetainedRange,
    pub evicted: Evicted,
    /// A terminal record was seen in the retained part.
    pub source_final: bool,
    /// PPU schema only (combined sources are reported per family).
    pub source_complete: Option<bool>,
    pub ppu: Ppu,
    pub spu: Option<Spu>,
    pub auto: Option<Auto>,
    pub fifo: Option<Fifo>,
}

/// One opened rolling file.
#[derive(Clone, Debug)]
pub struct RollingDoc {
    pub path: PathBuf,
    pub limits: OpenLimits,
    pub summary: Summary,
}

/// The page's standing statement about scope.
pub const NOTE: &str = "Retained window of a rolling capture. Every number below covers the retained segments only; \
the dropped prefix is a checkpoint claim, not authenticated history, and no whole-capture totals or verdict are reconstructed.";

/// Reads a coherent saved file of at most `cap` bytes (refused before reading when larger).
pub fn read_bounded(path: &Path, cap: u64) -> Result<Vec<u8>, String> {
    let file = std::fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let meta = file.metadata().map_err(|e| format!("{}: {e}", path.display()))?;
    if !meta.is_file() {
        return Err("a saved regular file is required".into());
    }
    if meta.len() > cap {
        return Err(format!(
            "file is {} bytes, above the viewer's rolling file limit of {} bytes",
            meta.len(),
            cap
        ));
    }
    let size = usize::try_from(meta.len()).map_err(|_| "file size does not fit this host".to_string())?;
    let mut bytes = Vec::new();
    bytes.try_reserve_exact(size).map_err(|e| format!("cannot reserve {size} bytes: {e}"))?;
    bytes.resize(size, 0);
    (&file).read_exact(&mut bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut extra = [0u8; 1];
    let more = (&file).read(&mut extra).map_err(|e| format!("{}: {e}", path.display()))?;
    if more != 0 || file.metadata().map(|m| m.len()).ok() != Some(meta.len()) {
        return Err("file changed while reading; open a coherent saved snapshot".into());
    }
    Ok(bytes)
}

/// Opens a rolling file from disk under `limits` (bounded read, then [`analyse`]).
pub fn open(path: &Path, limits: OpenLimits) -> Result<RollingDoc, String> {
    let bytes = read_bounded(path, limits.file_bytes)?;
    let summary = analyse(&bytes, limits)?;
    Ok(RollingDoc { path: path.to_path_buf(), limits, summary })
}

fn state_label(state: u32) -> &'static str {
    match state {
        1 => "OPEN",
        2 => "SEALED",
        3 => "STOPPED_RESOURCE",
        _ => "UNSPECIFIED",
    }
}

fn ppu_rows(p: &rolling::RetainedPpu) -> Ppu {
    Ppu {
        zones: p
            .zones
            .iter()
            .map(|z| ZoneRow {
                name: z.name.clone(),
                completed: z.completed,
                left_censored: z.left_censored,
                open: z.open,
                total_ticks: z.total_ticks,
            })
            .collect(),
        lanes: p
            .lanes
            .iter()
            .map(|(id, l)| LaneRow {
                id: *id,
                prefix_written: l.prefix_written,
                retained_written: l.retained_written,
                retained_gap_sum: l.retained_gap_sum,
                final_present: l.final_entry.is_some(),
            })
            .collect(),
        incomplete: p.incomplete.iter().map(ToString::to_string).collect(),
    }
}

/// Validates and projects one rolling container. A refusal carries the
/// library's reason text unchanged; nothing partial is returned.
pub fn analyse(bytes: &[u8], limits: OpenLimits) -> Result<Summary, String> {
    let cap = limits.file_bytes;
    if bytes.len() as u64 > cap {
        return Err(format!(
            "file is {} bytes, above the viewer's rolling file limit of {} bytes",
            bytes.len(),
            cap
        ));
    }
    let store_limits = store::Limits { max_file_bytes: cap, max_slot_bytes: cap.min(u64::from(u32::MAX)) as u32 };
    let physical = store::read(bytes, store_limits).map_err(|e| format!("{e}"))?;
    let first = physical
        .segments
        .first()
        .ok_or_else(|| "empty rolling store has no semantic checkpoint".to_string())?;
    let magic = first.checkpoint.get(..8).ok_or_else(|| "short checkpoint".to_string())?;
    let v = &physical.view;
    let retained_bytes = physical.segments.iter().map(|s| s.payload.len() as u64).sum::<u64>();
    let mut out = Summary {
        schema: Schema::Ppu,
        file_bytes: bytes.len() as u64,
        physical: Physical {
            version: v.format_version,
            state: state_label(v.state),
            reason: v.reason,
            recovered: v.recovered,
            generation: v.generation,
            bound_payload_bytes: (v.format_version == 2).then_some(v.bound_payload_bytes),
        },
        retained: RetainedRange {
            first_chunk: v.first_chunk,
            next_chunk: v.next_chunk,
            segments: v.retained_segments,
            payload_bytes: retained_bytes,
            first_payload_byte: v.evicted_payload_bytes,
        },
        evicted: Evicted {
            chunks: v.evicted_chunks,
            payload_bytes: v.evicted_payload_bytes,
            segments: v.evicted_segments,
        },
        source_final: false,
        source_complete: None,
        ppu: Ppu { zones: Vec::new(), lanes: Vec::new(), incomplete: Vec::new() },
        spu: None,
        auto: None,
        fifo: None,
    };
    if magic == b"PFRCKP01" {
        let f = join::read_ppu(bytes, store_limits, rolling::Limits::default()).map_err(|e| format!("{e}"))?;
        let final_seen = !f
            .retained
            .incomplete
            .iter()
            .any(|r| matches!(r, Incomplete::NoTrailer | Incomplete::TornTail));
        out.source_final = final_seen;
        out.source_complete = Some(final_seen && f.retained.incomplete.is_empty());
        out.ppu = ppu_rows(&f.retained);
    } else if magic == b"PFRMIX01" {
        let semantic = limits.analysis;
        let options = mixed::ReportOptions { spu_profile_table: &[], causal_limits: Default::default() };
        let f = join::read_combined(bytes, store_limits, semantic, options).map_err(|e| e.to_string())?;
        let r = f.retained;
        out.schema = Schema::Combined;
        out.source_final = r.terminal;
        out.ppu = ppu_rows(&r.ppu);
        out.spu = Some(Spu {
            note: r.spu.causal.note.to_string(),
            refusal: r.spu.causal.refusal.as_ref().map(|x| format!("{x:?}")),
            sources: r
                .spu
                .retained
                .sources
                .iter()
                .map(|(key, s)| SpuSource {
                    source: format!("{key:?}"),
                    retained_events: s.retained_events,
                    prefix_events: s.prefix_events,
                    retained_lost: s.retained_lost,
                    pairs: s.pairs.len(),
                    left_censored: s.left_censored.len(),
                    open_retained: s.open_retained.len(),
                    final_state: s.final_state,
                    raw_valid: s.raw_valid,
                    structure: format!("{:?}", s.structure),
                    protocol_error: s.protocol_error.clone(),
                })
                .collect(),
        });
        if let Some(a) = r.auto_report {
            let mut map_line = match &a.map {
                ps3dk_prof::auto_rsx::MapState::Qualified(_) => "qualified".to_string(),
                ps3dk_prof::auto_rsx::MapState::Unqualified(why) => format!("unqualified: {why}"),
            };
            if a.retained_support{map_line=format!("retained-support: {map_line}");}
            out.auto = Some(Auto {
                map: format!("{:?}", a.map),
                map_line,
                frames: a
                    .frames
                    .iter()
                    .map(|f| AutoFrame {
                        owner: f.view.owner,
                        frame: f.view.frame,
                        verdict: format!("{:?}", f.view.verdict),
                        verdict_label: f.view.verdict.label(),
                        reason: f.view.reason.clone(),
                        rsx_busy_raw: f.view.rsx_busy_raw,
                        rsx_busy_ppu: f.view.rsx_busy_ppu,
                        unavailable: f.unavailable.iter().map(|u| format!("{u:?}")).collect(),
                    })
                    .collect(),
            });
        }
        if let Some(fi) = r.fifo {
            out.fifo = Some(Fifo {
                final_seen: fi.final_seen,
                retained_events: fi.retained.events,
                retained_accounting_rows: fi.retained.accounting_rows,
                unavailable_window_rows:fi.unavailable_window_rows,
                retained_unavailable_window_rows:fi.unavailable_window_sequences.len(),
                known_missing: fi.known_missing,
                prefix_unassociated: fi.prefix_unassociated,
                boundary_frames: fi.boundary_frames.iter().map(|b| (b.owner, b.first, b.last)).collect(),
            });
        }
    } else {
        return Err("unsupported rolling checkpoint schema".into());
    }
    Ok(out)
}

/// The RSX verdict tally of the retained AUTO frames, in first-seen order (retained frames only).
pub fn verdict_counts(a: &Auto) -> Vec<(&'static str, usize)> {
    let mut out: Vec<(&'static str, usize)> = Vec::new();
    for f in &a.frames {
        match out.iter_mut().find(|(l, _)| *l == f.verdict_label) {
            Some(e) => e.1 += 1,
            None => out.push((f.verdict_label, 1)),
        }
    }
    out
}

/// The identity/store rows shown on the page, each labelled with its scope.
pub fn store_rows(d: &RollingDoc) -> Vec<(String, String)> {
    let s = &d.summary;
    let mut rows = vec![
        ("File".to_string(), d.path.display().to_string()),
        ("File bytes".to_string(), s.file_bytes.to_string()),
        (
            "Source schema".to_string(),
            match s.schema {
                Schema::Ppu => "PPU/WINDOWS".to_string(),
                Schema::Combined => "combined PPU/SPU (+ AUTO/FIFO when present)".to_string(),
            },
        ),
        ("Container version".to_string(), s.physical.version.to_string()),
        (
            "Physical state".to_string(),
            if s.physical.state == "STOPPED_RESOURCE" {
                format!("{} (reason {})", s.physical.state, s.physical.reason)
            } else {
                s.physical.state.to_string()
            },
        ),
        ("Root generation".to_string(), s.physical.generation.to_string()),
        ("Recovered root chain".to_string(), if s.physical.recovered { "yes" } else { "no" }.to_string()),
        ("Retained segments".to_string(), s.retained.segments.to_string()),
        (
            "Committed chunk frontier (retained)".to_string(),
            format!("{}..{}", s.retained.first_chunk, s.retained.next_chunk),
        ),
        ("Retained payload bytes".to_string(), s.retained.payload_bytes.to_string()),
        ("First retained payload byte".to_string(), s.retained.first_payload_byte.to_string()),
        (
            "Dropped prefix (claim)".to_string(),
            format!(
                "{} segments, {} chunks, {} payload bytes",
                s.evicted.segments, s.evicted.chunks, s.evicted.payload_bytes
            ),
        ),
    ];
    if let Some(b) = s.physical.bound_payload_bytes {
        rows.push(("Bound payload bytes (dropped + retained)".to_string(), b.to_string()));
    }
    rows.push((
        "Terminal record in retained part".to_string(),
        if s.source_final { "seen" } else { "not seen" }.to_string(),
    ));
    rows.push((
        "Analysis limits".to_string(),
        format!(
            "file {} bytes; analysis storage {} bytes; analysis work {} units",
            d.limits.file_bytes, d.limits.analysis.temporary_bytes, d.limits.analysis.work_items
        ),
    ));
    rows
}

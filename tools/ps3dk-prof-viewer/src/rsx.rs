//! The RSX frames page model ("Capture RSX frame", B1): the captures FOUND IN THE DATA, as the reader classified
//! them (`ps3dk_prof::rsx_capture`), with no egui types.  [`RsxSlot`] on the Document builds it once, on first need.
//! Draw rows are built only for the rows being drawn, from the reader's pair indices.  Raw RSX timestamps only: no
//! PPU time mapping on this page.  There is no request button and no protocol here (the live request is a later
//! slice); the page lists what the file holds.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

use ps3dk_prof::rsx_capture::{draw_kind, event_at, rsx_captures, CaptureState, RsxCapture, RsxCaptures, DRAW_CAP};
use ps3dk_prof::Analysis;

/// Draw rows a capture's list may hold: the reader's per-capture cap (it refuses larger captures).
pub const MAX_DRAW_ROWS: usize = DRAW_CAP as usize;
/// Draw rows built and painted per repaint: the visible window is clamped to it, and the omission is stated.
pub const PAINT_ROWS: usize = 256;
/// Caps beyond the analysis bound named on the page; the rest are counted.
pub const SHOWN_EXCESS_CAPS: usize = 16;

/// One capture's summary row.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CaptureRow {
    pub cap: u32,
    pub frame: String,
    pub source: String,
    pub state: CaptureState,
    /// The full state line (state word and every reason).
    pub line: String,
    /// "N" draws listed, or the refusal.
    pub draws: String,
}

/// One draw, built on demand.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DrawRow {
    pub index: usize,
    /// The ordinal, or "UNKNOWN".
    pub ordinal: String,
    pub kind: String,
    pub begin_raw: String,
    pub end_raw: String,
}

/// The page model of one Document.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RsxView {
    pub caps: RsxCaptures,
    pub rows: Vec<CaptureRow>,
    /// The file has at least one GPU source (the page is offered).
    pub has_gpu: bool,
    /// The caps beyond the analysis bound, as one line (at most [`SHOWN_EXCESS_CAPS`] named, the rest counted).
    pub excess_line: Option<String>,
    /// Every kept anomaly as text, and the "more not listed" line (built once, never per repaint).
    pub anomaly_lines: Vec<String>,
}

fn source_text(a: &Analysis, c: &RsxCapture) -> String {
    let x = a.capture.ext.as_ref();
    match c.source.and_then(|i| x.and_then(|x| x.sources.get(i))) {
        Some(s) => s.key.to_string(),
        None => "no marker placed".into(),
    }
}

pub fn build(a: &Analysis) -> RsxView {
    let caps = rsx_captures(a);
    let rows = caps
        .captures
        .iter()
        .map(|c| CaptureRow {
            cap: c.cap,
            frame: c.frame().map_or_else(|| "?".into(), |f| f.to_string()),
            source: source_text(a, c),
            state: c.state,
            line: c.state_line(),
            draws: if c.attribution_unknown {
                "UNKNOWN".into()
            } else if c.draws.is_empty() && c.draw_pairs > 0 {
                format!("{} (not listed)", c.draw_pairs)
            } else {
                c.draws.len().to_string()
            },
        })
        .collect();
    let has_gpu = a
        .capture
        .ext
        .as_ref()
        .is_some_and(|x| x.sources.iter().any(|s| matches!(s.key, ps3dk_prof::ext::SourceKey::Gpu(_))));
    let excess = caps.not_analysed.len() as u64 + caps.not_analysed_omitted;
    let excess_line = (excess > 0).then(|| {
        let named: Vec<String> = caps.not_analysed.iter().take(SHOWN_EXCESS_CAPS).map(u32::to_string).collect();
        let rest = excess - named.len() as u64;
        format!(
            "Not analysed (more than {} captures in the file): cap {}{}",
            ps3dk_prof::rsx_capture::MAX_CAPTURES,
            named.join(", "),
            if rest > 0 { format!(" and {rest} more") } else { String::new() }
        )
    });
    let mut anomaly_lines: Vec<String> = caps.anomalies.iter().map(|a| a.to_string()).collect();
    if caps.anomalies_omitted > 0 {
        anomaly_lines.push(format!("{} more anomalies not listed", caps.anomalies_omitted));
    }
    RsxView { caps, rows, has_gpu, excess_line, anomaly_lines }
}

impl RsxView {
    /// The Overview's line, or None when the file has no capture and no anomaly.
    pub fn overview_line(&self) -> Option<String> {
        let n = self.caps.captures.len() as u64 + self.caps.not_analysed.len() as u64 + self.caps.not_analysed_omitted;
        let m = self.caps.anomaly_count();
        if n == 0 && m == 0 {
            return None;
        }
        let fin = self.rows.iter().filter(|r| r.state == CaptureState::Finalized).count();
        Some(format!(
            "{n} RSX frame capture{} ({fin} FINALIZED); {m} reserved-marker anomal{}",
            if n == 1 { "" } else { "s" },
            if m == 1 { "y" } else { "ies" }
        ))
    }

    /// Draw rows the list shows for capture `ci` and how many it omits (beyond [`MAX_DRAW_ROWS`]).
    pub fn listed(&self, ci: usize) -> (usize, usize) {
        let n = self.caps.captures.get(ci).map_or(0, |c| c.draws.len());
        (n.min(MAX_DRAW_ROWS), n.saturating_sub(MAX_DRAW_ROWS))
    }

    /// Draw `i` of capture `ci`, from the reader's pair index (None out of range).
    pub fn draw_row(&self, a: &Analysis, ci: usize, i: usize) -> Option<DrawRow> {
        let c = self.caps.captures.get(ci)?;
        let s = a.capture.ext.as_ref()?.sources.get(c.source?)?;
        let p = s.raw.pairs.get(*c.draws.get(i)? as usize)?;
        let raw = |seq| event_at(s, seq).map_or_else(|| "?".into(), |e| e.raw.to_string());
        Some(DrawRow {
            index: i,
            ordinal: c.ordinal(i).map_or_else(|| "UNKNOWN".into(), |o| o.to_string()),
            kind: draw_kind(a, p.desc).map_or("?", |k| k.name()).to_string(),
            begin_raw: raw(p.begin),
            end_raw: raw(p.end),
        })
    }
}

/// The Document's RSX frames cache: built once, on first need, never per repaint.
#[derive(Default)]
pub struct RsxSlot {
    cell: OnceLock<Arc<RsxView>>,
    /// How many times the view was built (tests: once per Document).
    pub builds: AtomicU64,
}

impl std::fmt::Debug for RsxSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RsxSlot").field("builds", &self.builds.load(Ordering::Relaxed)).finish()
    }
}

impl RsxSlot {
    pub fn get(&self, a: &Analysis) -> Arc<RsxView> {
        self.cell
            .get_or_init(|| {
                self.builds.fetch_add(1, Ordering::Relaxed);
                Arc::new(build(a))
            })
            .clone()
    }
}

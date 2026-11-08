//! View model for the viewer: everything the windows show, computed from the
//! production reader's `Analysis` with no egui types, so it is unit tested.
//!
//! The reader is the authority. Values arrive as `Metric`s and stay `Metric`s
//! until display: an unknown or refused value is shown as such, never as a
//! number, and sorts after every known value.

use ps3dk_prof::report::verdict_line;
use ps3dk_prof::{load, load_file, Analysis, Metric, Outcome, Verdict, ZoneStats};
use std::cmp::Ordering;
use std::path::PathBuf;

/// One opened `.prof` file.
pub struct Document {
    pub path: PathBuf,
    pub size: usize,
    /// The analysis mode it was built with: an accepted prefix allowed (true) or strict.
    pub prefix_ok: bool,
    pub state: DocState,
    /// The EXTENSION SOURCES view, computed ONCE when the document is built (never per repaint).  Every open,
    /// replacement and live snapshot builds a new Document, so it cannot outlive the capture it describes.
    pub ext: ExtView,
    /// The Frames page index, also computed ONCE here; None for a MALFORMED document.
    pub frames: Option<crate::frames::FramesIndex>,
    /// The Frames page's SPU/RSX lanes: a one-entry cache filled on first need, so it never outlives this capture.
    pub foreign: crate::foreign::ForeignSlot,
    /// The Hot paths page, computed ONCE here (reader budgets bound it); None for a MALFORMED document.
    pub hot: Option<crate::hotpaths::HotView>,
    /// The Hot paths page's SPU/RSX lanes (F3b): a one-entry cache keyed by W32, filled on first need.
    pub foreign_hot: crate::foreign_hot::ForeignHotSlot,
    /// The Timeline's SPU/RSX rows: a one-entry cache keyed by W32, filled on first need.
    pub foreign_tl: crate::timeline_foreign::ForeignTimelineSlot,
    /// The RSX frames page (captures found in the data): built once, on first need.
    pub rsx: crate::rsx::RsxSlot,
}

/// What the Overview's EXTENSION SOURCES section shows.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ExtView {
    /// A v1 file or a MALFORMED one: no section.
    None,
    /// An extension file that declares no source: "no sources recorded; no SPU/RSX coverage is claimed".
    Empty,
    /// One block per source, in the reader's order: its facts and, for an SPU lane, its own epoch rows.
    Sources(Vec<ExtBlock>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtBlock {
    pub source: ps3dk_prof::report::SourceParts,
    pub epochs: ps3dk_prof::report::LaneEpochs,
}

pub fn ext_view(a: &Analysis) -> ExtView {
    use ps3dk_prof::report::{epoch_parts, source_parts};
    let Some(x) = &a.capture.ext else {
        return ExtView::None;
    };
    if x.sources.is_empty() {
        return ExtView::Empty;
    }
    ExtView::Sources(
        x.sources
            .iter()
            .map(|s| ExtBlock {
                source: source_parts(s),
                epochs: epoch_parts(x, s),
            })
            .collect(),
    )
}

pub enum DocState {
    /// The reader refused the file; `diagnosis` names the rule broken.
    Malformed {
        diagnosis: String,
    },
    Analysed(Box<Analysis>),
}

/// File names the viewer keeps for a session that failed a check: never analysed as an accepted prefix.
pub const NO_PREFIX_SUFFIX: &str = ".incomplete.prof";

impl Document {
    /// Opens `bytes`: a capture that ended early (no TRAILER, otherwise clean up to its last whole chunk) is
    /// analysed as an accepted prefix, except a file named `*.incomplete.prof` (any case), which keeps the strict
    /// analysis.
    pub fn from_bytes(path: PathBuf, bytes: &[u8]) -> Document {
        // Case-insensitive: Windows file names are, so `a.INCOMPLETE.PROF` is the same kept copy.
        let prefix_ok = !path.to_string_lossy().to_ascii_lowercase().ends_with(NO_PREFIX_SUFFIX);
        Document::build(path, bytes, prefix_ok)
    }

    /// The strict analysis only (a live session whose outcome failed a check).
    pub fn strict(path: PathBuf, bytes: &[u8]) -> Document {
        Document::build(path, bytes, false)
    }

    fn build(path: PathBuf, bytes: &[u8], prefix_ok: bool) -> Document {
        let loaded = if prefix_ok { load_file(bytes) } else { load(bytes) };
        let (state, ext, frames, hot) = match loaded {
            Outcome::Malformed(m) => (
                DocState::Malformed {
                    diagnosis: m.to_string(),
                },
                ExtView::None,
                None,
                None,
            ),
            Outcome::Analysed(a) => {
                let ext = ext_view(&a);
                let frames = crate::frames::FramesIndex::build(&a);
                let pm = ps3dk_prof::paths::paths(&a, &ps3dk_prof::paths::PathBudgets::default());
                let hot = crate::hotpaths::build(&a, &pm);
                (DocState::Analysed(a), ext, Some(frames), Some(hot))
            }
        };
        Document {
            path,
            size: bytes.len(),
            prefix_ok,
            state,
            ext,
            frames,
            foreign: crate::foreign::ForeignSlot::default(),
            hot,
            foreign_hot: crate::foreign_hot::ForeignHotSlot::default(),
            foreign_tl: crate::timeline_foreign::ForeignTimelineSlot::default(),
            rsx: crate::rsx::RsxSlot::default(),
        }
    }

    pub fn analysis(&self) -> Option<&Analysis> {
        match &self.state {
            DocState::Analysed(a) => Some(a),
            DocState::Malformed { .. } => None,
        }
    }

    /// The title-bar verdict, exactly as `ps3dk-prof check` words it.
    pub fn verdict(&self) -> VerdictView {
        match &self.state {
            DocState::Malformed { diagnosis } => {
                // The reader's own text already begins "MALFORMED (rule)".
                VerdictView {
                    tone: VerdictTone::Malformed,
                    text: diagnosis.clone(),
                }
            }
            DocState::Analysed(a) => match &a.prefix {
                // Cause-neutral: the bytes cannot say why the capture ended early.
                Some(_) => VerdictView {
                    tone: VerdictTone::CompleteEarly,
                    text: ps3dk_prof::report::verdict_text(a),
                },
                None => VerdictView {
                    tone: match a.verdict {
                        Verdict::Complete => VerdictTone::Complete,
                        Verdict::Incomplete(_) => VerdictTone::Incomplete,
                    },
                    text: verdict_line(&a.verdict),
                },
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VerdictTone {
    Complete,
    /// An accepted prefix: a capture that ended early, analysed up to its last whole chunk.
    CompleteEarly,
    Incomplete,
    Malformed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerdictView {
    pub tone: VerdictTone,
    pub text: String,
}

/// Tick-to-time conversion with the capture's own CLOCKS frequency. Without a
/// usable clock, conversion is unavailable and only ticks are shown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TimeBase {
    pub freq_hz: Option<u64>,
}

impl TimeBase {
    pub fn of(a: &Analysis) -> TimeBase {
        TimeBase {
            freq_hz: a
                .capture
                .clock
                .as_ref()
                .map(|c| c.freq_hz)
                .filter(|&f| f > 0),
        }
    }

    pub fn ms(&self, ticks: f64) -> Option<f64> {
        self.freq_hz.map(|f| ticks * 1000.0 / f as f64)
    }

    /// "13.305 ms (1061712 ticks)", or "1061712 ticks" without a clock.
    pub fn ticks(&self, ticks: u128) -> String {
        match self.ms(ticks as f64) {
            Some(ms) => format!("{} ({ticks} ticks)", format_ms(ms)),
            None => format!("{ticks} ticks"),
        }
    }

    pub fn ticks_f(&self, ticks: f64) -> String {
        match self.ms(ticks) {
            Some(ms) => format!("{} ({ticks:.1} ticks)", format_ms(ms)),
            None => format!("{ticks:.1} ticks"),
        }
    }

    /// The compact form for table cells: the time alone, or the ticks
    /// without a clock. The exact tick value goes in the cell's tooltip.
    pub fn short(&self, ticks: f64) -> String {
        match self.ms(ticks) {
            Some(ms) => format_ms(ms),
            None => format!("{ticks} ticks"),
        }
    }
}

pub fn format_ms(ms: f64) -> String {
    if ms >= 1000.0 {
        format!("{:.3} s", ms / 1000.0)
    } else if ms >= 1.0 {
        format!("{ms:.3} ms")
    } else {
        format!("{:.3} us", ms * 1000.0)
    }
}

/// Display a metric: a value through `show`, otherwise the reader's reason.
pub fn metric_text<T>(m: &Metric<T>, show: impl Fn(&T) -> String) -> String {
    match m {
        Metric::Value(v) => show(v),
        Metric::Unknown(why) => format!("unknown ({why})"),
        Metric::Refused(why) => format!("refused ({why})"),
    }
}

// ---- Overview --------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneRow {
    pub incarnation: u32,
    pub name: String,
    pub written: u64,
    pub frames: usize,
    pub spans: usize,
    pub valid: bool,
    pub loss: bool,
    pub reasons: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Overview {
    /// (label, value) pairs for the header and clock.
    pub identity: Vec<(String, String)>,
    pub minor_warning: bool,
    pub lanes: Vec<LaneRow>,
    /// (label, value) pairs from the trailer; empty when there is none.
    pub trailer: Vec<(String, String)>,
}

pub fn overview(a: &Analysis) -> Overview {
    let cap = &a.capture;
    let mut identity = Vec::new();
    match &cap.header {
        Some(h) => {
            let abi = match h.abi {
                1 => "ILP32".to_string(),
                2 => "LP64".to_string(),
                n => format!("unknown ({n})"),
            };
            let mode = match h.consumer_mode {
                1 => "APP".to_string(),
                2 => "THREAD".to_string(),
                n => format!("unknown ({n})"),
            };
            identity.push(("App".into(), h.app.clone()));
            identity.push(("Build id".into(), printable(&h.build_id)));
            identity.push(("SDK".into(), h.sdk_version.clone()));
            identity.push(("Compiler".into(), h.compiler_identity.clone()));
            identity.push(("Flags".into(), h.flags.clone()));
            identity.push(("Workload".into(), h.workload_tag.clone()));
            identity.push(("ABI".into(), abi));
            identity.push(("Drain mode".into(), mode));
            identity.push(("Session".into(), format!("0x{:016x}", h.session_id)));
            identity.push(("Ring records".into(), h.ring_records.to_string()));
            identity.push(("Max threads".into(), h.max_threads.to_string()));
        }
        None => identity.push(("Header".into(), "absent".into())),
    }
    identity.push((
        "Clock".into(),
        match &cap.clock {
            Some(c) if c.freq_hz > 0 => format!("PPU timebase {} Hz", c.freq_hz),
            _ => "unavailable: times are shown in ticks".to_string(),
        },
    ));
    let lanes = cap
        .lanes
        .iter()
        .map(|(&inc, lane)| {
            let view = a.lanes.get(&inc);
            LaneRow {
                incarnation: inc,
                name: lane
                    .info
                    .as_ref()
                    .map(|t| t.name.clone())
                    .unwrap_or_default(),
                written: lane.written,
                frames: lane.frames.len(),
                spans: lane.spans.len(),
                valid: view.is_some_and(|v| v.valid),
                loss: view.is_some_and(|v| v.loss),
                reasons: view.map(|v| v.reasons.join("; ")).unwrap_or_default(),
            }
        })
        .collect();
    let trailer = match &cap.trailer {
        Some(t) => vec![
            ("End flags".into(), format!("0x{:x}", t.end_flags)),
            ("Sink errors".into(), t.sink_error_count.to_string()),
            ("First sink error".into(), t.first_sink_error.to_string()),
            (
                "Unregistered producers refused".into(),
                t.unregistered_refusals.to_string(),
            ),
            ("Descriptors".into(), t.descriptor_count.to_string()),
        ],
        None => Vec::new(),
    };
    Overview {
        identity,
        minor_warning: cap.minor_warning,
        lanes,
        trailer,
    }
}

/// The exact bytes as hex, always, plus the text when every byte is printable:
/// the text alone could not tell "abcd" from the bytes 0xab 0xcd.
pub fn printable(bytes: &[u8]) -> String {
    let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    if bytes.is_empty() {
        "(empty)".to_string()
    } else if bytes.iter().all(|b| (0x20..0x7f).contains(b)) {
        format!("text \"{}\" (hex {hex})", String::from_utf8_lossy(bytes))
    } else {
        format!("hex {hex}")
    }
}

// ---- Zones -----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZoneColumn {
    Name,
    Instances,
    Completed,
    Censored,
    Total,
    Mean,
    P50,
    P90,
    P99,
    Max,
    Note,
}

impl ZoneColumn {
    pub const ALL: [ZoneColumn; 11] = [
        ZoneColumn::Name,
        ZoneColumn::Instances,
        ZoneColumn::Completed,
        ZoneColumn::Censored,
        ZoneColumn::Total,
        ZoneColumn::Mean,
        ZoneColumn::P50,
        ZoneColumn::P90,
        ZoneColumn::P99,
        ZoneColumn::Max,
        ZoneColumn::Note,
    ];

    /// Every duration column is inclusive (children included), as in `report`.
    pub fn title(self) -> &'static str {
        match self {
            ZoneColumn::Name => "Zone",
            ZoneColumn::Instances => "Instances",
            ZoneColumn::Completed => "Completed",
            ZoneColumn::Censored => "Censored",
            ZoneColumn::Total => "Total (inclusive)",
            ZoneColumn::Mean => "Mean (inclusive)",
            ZoneColumn::P50 => "p50 (incl.)",
            ZoneColumn::P90 => "p90 (incl.)",
            ZoneColumn::P99 => "p99 (incl.)",
            ZoneColumn::Max => "Max (incl.)",
            ZoneColumn::Note => "Qualification",
        }
    }
}

/// A zone's identity is the reader's logical key (name, logical tag), not the
/// name alone: two zones may share a name.
pub type ZoneKey = (String, String);

pub fn zone_key(z: &ZoneStats) -> ZoneKey {
    (z.name.clone(), z.logical_tag.clone())
}

pub fn zone_label(z: &ZoneStats) -> String {
    if z.logical_tag.is_empty() {
        z.name.clone()
    } else {
        format!("{} [{}]", z.name, z.logical_tag)
    }
}

/// The visible qualification of a row's values: the reader's "completed
/// instances only" label, and how many ended instances had no duration
/// (END before BEGIN). Empty when every instance counted.
pub fn zone_note(z: &ZoneStats) -> String {
    let mut parts = Vec::new();
    if let Some(label) = &z.label {
        parts.push(label.clone());
    }
    if z.unavailable > 0 {
        parts.push(format!(
            "{} unavailable (END before BEGIN), excluded",
            z.unavailable
        ));
    }
    parts.join("; ")
}

/// Zone statistics from valid lanes only, exactly as the reader computed them.
pub fn zones(a: &Analysis) -> Vec<ZoneStats> {
    a.zones.clone()
}

/// The `file:line` sites of a zone's descriptors (several when a logical key is
/// shared, which the reader also reports).
pub fn zone_sites(a: &Analysis, z: &ZoneStats) -> Vec<String> {
    a.capture
        .descriptors
        .values()
        .filter(|d| d.name == z.name && d.logical_tag == z.logical_tag)
        .map(|d| format!("{}:{}", d.file, d.line))
        .collect()
}

/// The cell text: durations in their compact form.
pub fn zone_cell(z: &ZoneStats, col: ZoneColumn, tb: TimeBase) -> String {
    match col {
        ZoneColumn::Name => zone_label(z),
        ZoneColumn::Note => zone_note(z),
        ZoneColumn::Instances => z.instances.to_string(),
        ZoneColumn::Completed => z.completed.to_string(),
        ZoneColumn::Censored => z.censored.to_string(),
        ZoneColumn::Total => metric_text(&z.total, |v| tb.short(*v as f64)),
        ZoneColumn::Mean => metric_text(&z.mean, |v| tb.short(*v)),
        ZoneColumn::P50 => metric_text(&z.p50, |v| tb.short(*v as f64)),
        ZoneColumn::P90 => metric_text(&z.p90, |v| tb.short(*v as f64)),
        ZoneColumn::P99 => metric_text(&z.p99, |v| tb.short(*v as f64)),
        ZoneColumn::Max => metric_text(&z.max, |v| tb.short(*v as f64)),
    }
}

/// The exact value behind a duration cell (time and exact ticks), for its
/// tooltip; None for columns that are not durations.
pub fn zone_cell_exact(z: &ZoneStats, col: ZoneColumn, tb: TimeBase) -> Option<String> {
    Some(match col {
        ZoneColumn::Total => metric_text(&z.total, |v| tb.ticks(*v)),
        ZoneColumn::Mean => metric_text(&z.mean, |v| tb.ticks_f(*v)),
        ZoneColumn::P50 => metric_text(&z.p50, |v| tb.ticks(u128::from(*v))),
        ZoneColumn::P90 => metric_text(&z.p90, |v| tb.ticks(u128::from(*v))),
        ZoneColumn::P99 => metric_text(&z.p99, |v| tb.ticks(u128::from(*v))),
        ZoneColumn::Max => metric_text(&z.max, |v| tb.ticks(u128::from(*v))),
        _ => return None,
    })
}

/// Known values compare by value; any unknown or refused value sorts after all
/// known ones (in both directions), so a missing number never ranks as small.
fn metric_order<T: PartialOrd>(a: &Metric<T>, b: &Metric<T>, descending: bool) -> Ordering {
    match (a.value(), b.value()) {
        (Some(x), Some(y)) => {
            let o = x.partial_cmp(y).unwrap_or(Ordering::Equal);
            if descending {
                o.reverse()
            } else {
                o
            }
        }
        (Some(_), None) => Ordering::Less,
        (None, Some(_)) => Ordering::Greater,
        (None, None) => Ordering::Equal,
    }
}

fn plain_order<T: Ord>(a: &T, b: &T, descending: bool) -> Ordering {
    if descending {
        b.cmp(a)
    } else {
        a.cmp(b)
    }
}

/// Stable sort; ties keep the reader's order.
pub fn sort_zones(rows: &mut [ZoneStats], col: ZoneColumn, descending: bool) {
    rows.sort_by(|a, b| match col {
        ZoneColumn::Name => plain_order(&zone_key(a), &zone_key(b), descending),
        ZoneColumn::Note => plain_order(&zone_note(a), &zone_note(b), descending),
        ZoneColumn::Instances => plain_order(&a.instances, &b.instances, descending),
        ZoneColumn::Completed => plain_order(&a.completed, &b.completed, descending),
        ZoneColumn::Censored => plain_order(&a.censored, &b.censored, descending),
        ZoneColumn::Total => metric_order(&a.total, &b.total, descending),
        ZoneColumn::Mean => metric_order(&a.mean, &b.mean, descending),
        ZoneColumn::P50 => metric_order(&a.p50, &b.p50, descending),
        ZoneColumn::P90 => metric_order(&a.p90, &b.p90, descending),
        ZoneColumn::P99 => metric_order(&a.p99, &b.p99, descending),
        ZoneColumn::Max => metric_order(&a.max, &b.max, descending),
    });
}

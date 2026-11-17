//! View model for the Automatic page: per-frame AUTO verdicts, RSX span/busy,
//! exclusions, steady-range medians and mix, computed strictly from the reader's
//! `ps3dk_prof::auto_rsx::analyze`.

use ps3dk_prof::auto_rsx::{self, Config, FrameView, Report, Verdict, EXCLUSIONS, KINDS};
use ps3dk_prof::model::Capture;

/// The Automatic page view model, holding the analyzed AUTO report and capture clock properties.
#[derive(Debug, Clone)]
pub struct AutoView {
    pub report: Report,
    pub tb_freq: u64,
    /// Absolute intervals mapped and merged by the reader, cached with this report.
    pub busy: auto_rsx::BusyTimeline,
}

/// The Passes page view model: the reader's by-tag and per-range inventory and
/// the whole-run summaries (`tag_summary` / `range_summary`). Numbers are never
/// re-derived here; they come straight from `ps3dk_prof::auto_rsx`.
#[derive(Debug, Clone, Default)]
pub struct PassesView {
    pub by: auto_rsx::ByTag,
    /// Per tag: `(frames seen, ops, busy sum, busy median, busy p95)`.
    pub tags: std::collections::BTreeMap<u32, (u64, u64, u64, u64, u64)>,
    /// Per range name: `(frames seen, ranges, raw sum, raw median, raw p95)`.
    pub ranges: std::collections::BTreeMap<String, (u64, u64, u64, u64, u64)>,
    /// The capture's RSX clock map (reader-qualified or not); None without AUTO data.
    pub map: Option<auto_rsx::MapState>,
    /// PPU timebase frequency from the capture's clock record; None when absent.
    pub tb_freq: Option<u64>,
}

/// Builds the Passes page view from a capture via the reader only.
pub fn passes_build(cap: &Capture) -> PassesView {
    let by = auto_rsx::by_tag(cap);
    let tags = auto_rsx::tag_summary(&by);
    let ranges = auto_rsx::range_summary(&by);
    let map = cap.auto.as_ref().map(auto_rsx::qualify);
    let tb_freq = cap.clock.as_ref().map(|c| c.freq_hz).filter(|&f| f > 0);
    PassesView { by, tags, ranges, map, tb_freq }
}

/// Builds the Passes page view scoped to a capture window's frames.  Uses the
/// reader's `by_tag_window` (`RangeOccurrence` and the per-name sums are
/// rebuilt only over the window's frames).
pub fn passes_build_window(cap: &Capture, w: &ps3dk_prof::CaptureWindow) -> PassesView {
    let by = auto_rsx::by_tag_window(cap, w);
    let tags = auto_rsx::tag_summary(&by);
    let ranges = auto_rsx::range_summary(&by);
    let map = cap.auto.as_ref().map(auto_rsx::qualify);
    let tb_freq = cap.clock.as_ref().map(|c| c.freq_hz).filter(|&f| f > 0);
    PassesView { by, tags, ranges, map, tb_freq }
}

impl PassesView {
    /// Whether the capture's AUTO writer has the tag capability (CONFIG 1023/11).
    pub fn has_tags(&self) -> bool {
        self.by.tagged
    }

    /// A raw RSX duration as shown text and hover text. Milliseconds (3 decimals) only through the reader's
    /// qualified clock map and the capture's own timebase; otherwise "unavailable" with the reason on hover.
    pub fn duration(&self, raw: u64) -> (String, String) {
        let bounds = self.map.as_ref().and_then(|m| auto_rsx::raw_duration_ppu_bounds(m, raw));
        match (bounds, self.tb_freq) {
            (Some((lo, hi)), Some(f)) => {
                let ms = |t: u64| t as f64 / f as f64 * 1000.0;
                let text = if lo == hi { format!("{:.3} ms", ms(lo)) } else { format!("{:.3}..{:.3} ms", ms(lo), ms(hi)) };
                (text, format!("{raw} raw RSX ticks = {lo}..{hi} PPU ticks (clock map bounds)"))
            }
            (None, _) => ("unavailable".into(), format!("{raw} raw RSX ticks; the RSX clock map is not qualified, so no time is claimed")),
            (_, None) => ("unavailable".into(), format!("{raw} raw RSX ticks; the capture has no PPU timebase record")),
        }
    }
}

impl AutoView {
    /// Builds the Automatic page view from a capture.
    pub fn build(cap: &Capture) -> AutoView {
        let report = auto_rsx::analyze(cap);
        let tb_freq = cap
            .clock
            .as_ref()
            .map(|c| c.freq_hz)
            .filter(|&f| f > 0)
            .unwrap_or(79_800_000);
        let busy = auto_rsx::busy_timeline(cap, &report);
        AutoView { report, tb_freq, busy }
    }

    /// The Automatic page view scoped to capture window `w`, exactly as the
    /// CLI's `auto --window` renders it: the report filtered by the window
    /// (`Report::filter_window`) and the RSX busy intervals confined to the
    /// window's frames (`w.contains_frame`).  Analyzes the capture exactly
    /// once; the reader's `busy_timeline` needs the full report for its
    /// map/refusal checks, so we build busy from it and then retain only the
    /// intervals this window holds.
    pub fn build_window(cap: &Capture, w: &ps3dk_prof::CaptureWindow) -> AutoView {
        let full = auto_rsx::analyze(cap);
        let tb_freq = cap
            .clock
            .as_ref()
            .map(|c| c.freq_hz)
            .filter(|&f| f > 0)
            .unwrap_or(79_800_000);
        let report = full.filter_window(w);
        let mut busy = auto_rsx::busy_timeline(cap, &full);
        busy.bars.retain(|b| w.contains_frame(b.frame));
        AutoView { report, tb_freq, busy }
    }

    /// Whether this capture contains automatic profiler capability (FLAG_AUTO / auto chunks).
    pub fn has_auto(&self) -> bool {
        self.report.capability
    }

    /// Whether the AUTO status is marked FINAL.
    pub fn is_final(&self) -> bool {
        self.report.status.as_ref().map_or(false, |s| s.is_final())
    }

    /// Whether any RSX frame boundary marks were missed by the writer (status flag ST_FRAME_MARK_MISSED=128).
    pub fn frame_marks_missed(&self) -> bool {
        self.report
            .status
            .as_ref()
            .is_some_and(|s| s.flags & auto_rsx::ST_FRAME_MARK_MISSED != 0)
    }

    /// Coverage records lost to ring buffer capacity (counter 6, "lost" in coverage rows). For an unsealed capture
    /// this is the reader's capture total: the last STATUS value (a lower bound), or None (UNAVAILABLE) without one.
    pub fn coverage_rows_lost(&self) -> Option<u64> {
        match &self.report.unsealed {
            Some(u) => u.total("frames_lost").and_then(|t| t.value),
            None => self.report.status.as_ref().map(|s| s.counters[6]),
        }
    }

    /// The reader's unsealed-capture facts (no FINAL STATUS): the unrecorded tail and the capture totals.
    pub fn unsealed(&self) -> Option<&auto_rsx::Unsealed> {
        self.report.unsealed.as_ref()
    }

    /// Operations dropped past the retention ceiling.
    pub fn ops_dropped(&self) -> u64 {
        self.report.ops_dropped
    }

    /// The CONFIG's metadata record budget (present on an ATTACHED config) — the reader's
    /// "metadata {} ... records" value.
    pub fn metadata_records(&self) -> Option<u32> {
        self.report.config.as_ref().and_then(Config::attached).map(|a| a.metadata)
    }

    /// Converts PPU ticks to milliseconds with 3 decimals.
    pub fn ticks_to_ms(&self, ticks: u64) -> f64 {
        (ticks as f64 / self.tb_freq as f64) * 1000.0
    }

    /// Formats PPU ticks as "{:.3} ms".
    pub fn format_ms(&self, ticks: u64) -> String {
        format!("{:.3} ms", self.ticks_to_ms(ticks))
    }

    /// Frame interval in PPU ticks.
    pub fn frame_interval_ticks(f: &FrameView) -> u64 {
        f.tb_end.saturating_sub(f.tb_begin)
    }

    /// Frame interval in milliseconds.
    pub fn frame_interval_ms(&self, f: &FrameView) -> f64 {
        self.ticks_to_ms(Self::frame_interval_ticks(f))
    }

    /// Counts per verdict class across all frames in this capture.
    pub fn verdict_counts(&self) -> [(Verdict, usize); 5] {
        let mut rsx = 0;
        let mut ppu = 0;
        let mut pacing = 0;
        let mut mixed = 0;
        let mut unknown = 0;
        for f in &self.report.frames {
            match f.verdict {
                Verdict::RsxBound => rsx += 1,
                Verdict::PpuBound => ppu += 1,
                Verdict::Pacing => pacing += 1,
                Verdict::Mixed => mixed += 1,
                Verdict::Unknown => unknown += 1,
            }
        }
        [
            (Verdict::RsxBound, rsx),
            (Verdict::PpuBound, ppu),
            (Verdict::Pacing, pacing),
            (Verdict::Mixed, mixed),
            (Verdict::Unknown, unknown),
        ]
    }

    /// Summary of frames in the steady range `start..=end` (frame numbers).
    pub fn steady_summary(&self, start: u64, end: u64) -> SteadySummary {
        let (lo, hi) = if start <= end { (start, end) } else { (end, start) };
        let matching: Vec<&FrameView> = self
            .report
            .frames
            .iter()
            .filter(|f| f.frame >= lo && f.frame <= hi)
            .collect();
        SteadySummary::compute(&matching, self.tb_freq)
    }

    /// Min and max frame number in this capture (or (0, 0) if empty).
    pub fn frame_range(&self) -> (u64, u64) {
        if self.report.frames.is_empty() {
            return (0, 0);
        }
        let first = self.report.frames.first().map(|f| f.frame).unwrap_or(0);
        let last = self.report.frames.last().map(|f| f.frame).unwrap_or(0);
        (first.min(last), first.max(last))
    }
}

/// Computes the median of an unsigned 64-bit integer slice using overflow-safe arithmetic.
pub fn median_of(mut list: Vec<u64>) -> Option<u64> {
    if list.is_empty() {
        return None;
    }
    list.sort_unstable();
    let n = list.len();
    if n % 2 == 1 {
        Some(list[n / 2])
    } else {
        Some(((u128::from(list[n / 2 - 1]) + u128::from(list[n / 2])) / 2) as u64)
    }
}

/// Summary metrics for a selected steady frame range.
#[derive(Debug, Clone, PartialEq)]
pub struct SteadySummary {
    pub frame_count: usize,
    /// Counts in order: (RsxBound, PpuBound, Pacing, Mixed, Unknown)
    pub verdict_counts: [(Verdict, usize); 5],
    /// Median frame interval in PPU ticks
    pub median_interval_ticks: Option<u64>,
    /// Median RSX span in PPU ticks as (median_lo, median_hi) when clock map is qualified
    pub median_span_ppu_ticks: Option<(u64, u64)>,
    /// Median RSX span in raw ticks
    pub median_span_raw_ticks: Option<u64>,
    /// Median RSX busy in PPU ticks as (median_lo, median_hi) when clock map is qualified
    pub median_busy_ppu_ticks: Option<(u64, u64)>,
    /// Median RSX busy in raw ticks
    pub median_busy_raw_ticks: Option<u64>,
    pub tb_freq: u64,
}

impl SteadySummary {
    pub fn compute(frames: &[&FrameView], tb_freq: u64) -> SteadySummary {
        let frame_count = frames.len();
        let mut rsx = 0;
        let mut ppu = 0;
        let mut pacing = 0;
        let mut mixed = 0;
        let mut unknown = 0;
        for f in frames {
            match f.verdict {
                Verdict::RsxBound => rsx += 1,
                Verdict::PpuBound => ppu += 1,
                Verdict::Pacing => pacing += 1,
                Verdict::Mixed => mixed += 1,
                Verdict::Unknown => unknown += 1,
            }
        }
        let verdict_counts = [
            (Verdict::RsxBound, rsx),
            (Verdict::PpuBound, ppu),
            (Verdict::Pacing, pacing),
            (Verdict::Mixed, mixed),
            (Verdict::Unknown, unknown),
        ];

        let intervals: Vec<u64> = frames
            .iter()
            .map(|f| f.tb_end.saturating_sub(f.tb_begin))
            .collect();
        let median_interval_ticks = median_of(intervals);

        let spans_ppu_lo: Vec<u64> = frames.iter().filter_map(|f| f.rsx_span_ppu.map(|(lo, _)| lo)).collect();
        let spans_ppu_hi: Vec<u64> = frames.iter().filter_map(|f| f.rsx_span_ppu.map(|(_, hi)| hi)).collect();
        let median_span_ppu_ticks = match (median_of(spans_ppu_lo), median_of(spans_ppu_hi)) {
            (Some(lo), Some(hi)) => Some((lo, hi)),
            _ => None,
        };

        let spans_raw: Vec<u64> = frames.iter().filter_map(|f| f.rsx_span_raw).collect();
        let median_span_raw_ticks = median_of(spans_raw);

        let busy_ppu_lo: Vec<u64> = frames.iter().filter_map(|f| f.rsx_busy_ppu.map(|(lo, _)| lo)).collect();
        let busy_ppu_hi: Vec<u64> = frames.iter().filter_map(|f| f.rsx_busy_ppu.map(|(_, hi)| hi)).collect();
        let median_busy_ppu_ticks = match (median_of(busy_ppu_lo), median_of(busy_ppu_hi)) {
            (Some(lo), Some(hi)) => Some((lo, hi)),
            _ => None,
        };

        let busy_raw: Vec<u64> = frames.iter().filter_map(|f| f.rsx_busy_raw).collect();
        let median_busy_raw_ticks = median_of(busy_raw);

        SteadySummary {
            frame_count,
            verdict_counts,
            median_interval_ticks,
            median_span_ppu_ticks,
            median_span_raw_ticks,
            median_busy_ppu_ticks,
            median_busy_raw_ticks,
            tb_freq,
        }
    }

    /// Count of frames with the given verdict.
    pub fn count_of(&self, v: Verdict) -> usize {
        self.verdict_counts
            .iter()
            .find(|(verdict, _)| *verdict == v)
            .map_or(0, |(_, c)| *c)
    }

    /// Percentage of frames with the given verdict.
    pub fn mix_pct(&self, v: Verdict) -> f64 {
        if self.frame_count == 0 {
            0.0
        } else {
            (self.count_of(v) as f64 / self.frame_count as f64) * 100.0
        }
    }

    /// Median interval in milliseconds.
    pub fn median_interval_ms(&self) -> Option<f64> {
        self.median_interval_ticks
            .map(|t| (t as f64 / self.tb_freq as f64) * 1000.0)
    }

    /// Median RSX span in milliseconds as (lo, hi).
    pub fn median_span_ms(&self) -> Option<(f64, f64)> {
        self.median_span_ppu_ticks.map(|(lo, hi)| (
            (lo as f64 / self.tb_freq as f64) * 1000.0,
            (hi as f64 / self.tb_freq as f64) * 1000.0,
        ))
    }

    /// Median RSX busy in milliseconds as (lo, hi).
    pub fn median_busy_ms(&self) -> Option<(f64, f64)> {
        self.median_busy_ppu_ticks.map(|(lo, hi)| (
            (lo as f64 / self.tb_freq as f64) * 1000.0,
            (hi as f64 / self.tb_freq as f64) * 1000.0,
        ))
    }
}

/// Column selection for sorting the frame table.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FrameSortColumn {
    #[default]
    Frame,
    Verdict,
    Interval,
    Span,
    Busy,
    Excluded,
    CallCovered,
}

/// Filter for displaying frames by verdict class.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum VerdictFilter {
    #[default]
    All,
    RsxBound,
    PpuBound,
    Pacing,
    Mixed,
    Unknown,
}

impl VerdictFilter {
    pub fn matches(self, v: Verdict) -> bool {
        match self {
            VerdictFilter::All => true,
            VerdictFilter::RsxBound => v == Verdict::RsxBound,
            VerdictFilter::PpuBound => v == Verdict::PpuBound,
            VerdictFilter::Pacing => v == Verdict::Pacing,
            VerdictFilter::Mixed => v == Verdict::Mixed,
            VerdictFilter::Unknown => v == Verdict::Unknown,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            VerdictFilter::All => "All",
            VerdictFilter::RsxBound => "RSX_BOUND",
            VerdictFilter::PpuBound => "PPU_BOUND",
            VerdictFilter::Pacing => "PACING",
            VerdictFilter::Mixed => "MIXED",
            VerdictFilter::Unknown => "UNKNOWN",
        }
    }
}

/// Sorts a slice of frame references in place according to column and direction.
pub fn sort_frames(frames: &mut [&FrameView], col: FrameSortColumn, asc: bool) {
    frames.sort_by(|a, b| {
        let ord = match col {
            FrameSortColumn::Frame => a.frame.cmp(&b.frame),
            FrameSortColumn::Verdict => (a.verdict.key()).cmp(b.verdict.key()),
            FrameSortColumn::Interval => {
                let ia = a.tb_end.saturating_sub(a.tb_begin);
                let ib = b.tb_end.saturating_sub(b.tb_begin);
                ia.cmp(&ib)
            }
            FrameSortColumn::Span => {
                let sa = a.rsx_span_raw.unwrap_or(0);
                let sb = b.rsx_span_raw.unwrap_or(0);
                sa.cmp(&sb)
            }
            FrameSortColumn::Busy => {
                let ba = a.rsx_busy_raw.unwrap_or(0);
                let bb = b.rsx_busy_raw.unwrap_or(0);
                ba.cmp(&bb)
            }
            FrameSortColumn::Excluded => {
                let ea: u64 = a.excluded.iter().sum();
                let eb: u64 = b.excluded.iter().sum();
                ea.cmp(&eb)
            }
            FrameSortColumn::CallCovered => a.call_covered.cmp(&b.call_covered),
        };
        if asc {
            ord
        } else {
            ord.reverse()
        }
    });
}

/// Returns a breakdown of exclusions for this frame: (kind, reason, count) where count > 0.
pub fn exclusions_breakdown(f: &FrameView) -> Vec<(&'static str, &'static str, u64)> {
    let mut out = Vec::new();
    for (k_idx, kind) in KINDS.iter().enumerate() {
        for (c_idx, reason) in EXCLUSIONS.iter().enumerate() {
            let count = f.exclusions[k_idx][c_idx];
            if count > 0 {
                out.push((*kind, *reason, count));
            }
        }
    }
    out
}

/// Report-ring exclusion rows of one frame (2047/11): per kind, the covered visits left untimed
/// because no MAIN extent was free (`ring_full`) and those whose report-ring state was unknown or
/// whose recycling was refused (`ring_state`).  Disjoint from the ten `exclusions` columns;
/// all zero in older captures.
pub fn ring_exclusions_breakdown(f: &FrameView) -> Vec<(&'static str, &'static str, u64)> {
    let mut out = Vec::new();
    for (k_idx, kind) in KINDS.iter().enumerate() {
        for (reason, count) in [("ring full", f.ring_full[k_idx]), ("unknown ring state", f.ring_state[k_idx])] {
            if count > 0 {
                out.push((*kind, reason, count));
            }
        }
    }
    out
}

/// Presentation shares (flip-wait and polling cadence) computed against frame interval.
#[derive(Debug, Clone, PartialEq)]
pub struct PresentationShare {
    pub flip_wait_pct: Option<f64>,
    pub flip_wait_ticks: Option<u64>,
    pub flip_wait_scopes: Option<u32>,
    pub cadence_pct: Option<f64>,
    pub cadence_ticks: Option<u64>,
    pub cadence_runs: Option<u32>,
    pub legacy_polls_count: Option<u32>,
    pub legacy_polls_ticks: Option<u64>,
}

/// Calculates presentation shares for a frame against its interval.
pub fn presentation_share(f: &FrameView) -> Option<PresentationShare> {
    let p = f.presentation.as_ref()?;
    let interval = f.tb_end.saturating_sub(f.tb_begin);
    let pct = |ticks: u64| -> Option<f64> {
        if interval == 0 {
            None
        } else {
            Some((ticks as f64 / interval as f64) * 100.0)
        }
    };
    Some(PresentationShare {
        flip_wait_pct: p.flip_wait.and_then(|(t, _)| pct(t)),
        flip_wait_ticks: p.flip_wait.map(|(t, _)| t),
        flip_wait_scopes: p.flip_wait.map(|(_, n)| n),
        cadence_pct: p.cadence.and_then(|c| pct(c.ticks)),
        cadence_ticks: p.cadence.map(|c| c.ticks),
        cadence_runs: p.cadence.map(|c| c.runs),
        legacy_polls_count: p.legacy_polls.map(|(n, _)| n),
        legacy_polls_ticks: p.legacy_polls.map(|(_, t)| t),
    })
}

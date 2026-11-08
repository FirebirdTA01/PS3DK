//! Frames page graph display modes: the view model behind the mode selector (no egui types).
//!
//! Every mode shows the same facts as the bar view ([`crate::frames`] for the PPU owner, [`crate::foreign`] for the
//! SPU/RSX lanes) and nothing else:
//! - **Bars** (the default): the F1 PPU graph and the F2b lane strips, unchanged.
//! - **Lines**: one line per series on one shared tick axis, frame k at the same x for every series.  The PPU point
//!   is the exact elapsed value.  A lane point is the UPPER bound `hi` of its included-work bound (the conservative
//!   side for the budget), with the `lo..hi` range drawn as a translucent band; an infinite `hi` has no line point
//!   and draws its band to the top with the open-arrow cap.  The line BREAKS at every slot without a value (UNKNOWN,
//!   REFUSED, nothing placed, no interval): it never bridges a gap and never draws one as 0.
//! - **Distribution**: per series, measured frames only.  The PPU row is an exact integer histogram plus the
//!   ZoneStats-rule percentiles; a lane row gives, per percentile, the enclosing interval
//!   `[p-th smallest lo, p-th smallest hi]` (order statistics are monotone, so the percentile of the true per-frame
//!   values lies in it).  Excluded frames are counted, never binned.
//! - **Verdict map**: one row per series, one cell per frame, coloured by the existing budget verdict (OVER /
//!   WITHIN / UNDETERMINED) or by the slot kind; no new value.
//!
//! No mode sums series: the lanes run concurrently with the PPU and with each other, and a lane bar is
//! start-attributed, unclipped work, not occupancy, so neither a stacked total nor a per-frame "utilisation" ratio
//! is a fact the reader establishes.
//!
//! Per-repaint work is bounded by the visible slots: every function here takes a slot range (the visible columns,
//! plus one neighbour each side for the line joins) or the owner's cached order, never a capture-wide scan.

use std::collections::VecDeque;
use std::ops::Range;

use crate::foreign::{mark, LaneDist, LaneView, Mark, VerdictView};
use crate::frames::{over_budget, Budget, Owner, SlotKind};

/// The Frames page graph mode (in-memory per viewer; the app has no persisted settings store).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GraphMode {
    #[default]
    Bars,
    Lines,
    Distribution,
    VerdictMap,
}

impl GraphMode {
    pub const ALL: [GraphMode; 4] = [GraphMode::Bars, GraphMode::Lines, GraphMode::Distribution, GraphMode::VerdictMap];
    pub fn label(self) -> &'static str {
        match self {
            GraphMode::Bars => "Bars",
            GraphMode::Lines => "Lines",
            GraphMode::Distribution => "Distribution",
            GraphMode::VerdictMap => "Verdict map",
        }
    }
    /// Modes with one column per frame (shared x positions, horizontal scroll and click-to-select).
    pub fn per_frame(self) -> bool {
        !matches!(self, GraphMode::Distribution)
    }
}

/// The lines mode's moving-average window (raw lines are always drawn as well).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Smoothing {
    #[default]
    Off,
    W5,
    W15,
    W30,
}

impl Smoothing {
    pub const ALL: [Smoothing; 4] = [Smoothing::Off, Smoothing::W5, Smoothing::W15, Smoothing::W30];
    pub fn window(self) -> Option<usize> {
        match self {
            Smoothing::Off => None,
            Smoothing::W5 => Some(5),
            Smoothing::W15 => Some(15),
            Smoothing::W30 => Some(30),
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Smoothing::Off => "Raw",
            Smoothing::W5 => "Mean 5",
            Smoothing::W15 => "Mean 15",
            Smoothing::W30 => "Mean 30",
        }
    }
}

/// The largest moving-average window any caller may ask for: the per-repaint lookback cap.
pub const SMOOTH_MAX: usize = 64;

/// One slot of one series in lines mode.  Pixel values are ticks as f64 (pixels only; text uses the exact values).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LinePoint {
    /// A line point at `y` ticks.  `band` is a lane bound's (lo, lo_open): the range lo..y is drawn translucent.  The
    /// PPU value is exact and has no band.
    At { y: f64, band: Option<(f64, bool)> },
    /// A lane bound whose upper end is infinite (or not representable): no line point; the band runs from lo to
    /// the top with the open-arrow cap.
    Unbounded { lo: f64, lo_open: bool },
    /// A measured frame with nothing included on this lane (never 0).
    NonePlaced,
    /// The PPU slot is UNKNOWN (never 0, never interpolated).
    Unknown,
    /// The PPU slot is REFUSED.
    Refused,
    /// A lane slot with no measured interval (the PPU glyph explains the slot).
    NoInterval,
}

impl LinePoint {
    pub fn y(&self) -> Option<f64> {
        match *self {
            LinePoint::At { y, .. } => Some(y),
            _ => None,
        }
    }
}

/// The PPU point of owner slot `i` (out of range: no interval).
pub fn ppu_point(o: &Owner, i: usize) -> LinePoint {
    match o.slots.get(i).map(|s| &s.kind) {
        Some(SlotKind::Measured(d)) => LinePoint::At { y: *d as f64, band: None },
        Some(SlotKind::Unknown(_)) => LinePoint::Unknown,
        Some(SlotKind::Refused(_)) => LinePoint::Refused,
        None => LinePoint::NoInterval,
    }
}

/// The point of owner slot `i` on `lane`: the bound's finite upper end, with lo..hi as the band.
pub fn lane_point(o: &Owner, lane: &LaneView, i: usize) -> LinePoint {
    match mark(o, lane, i) {
        Mark::Bar(b) => match b.hi {
            Some(hi) if !b.hi_inf => LinePoint::At { y: hi, band: Some((b.lo, b.lo_open)) },
            _ => LinePoint::Unbounded { lo: b.lo, lo_open: b.lo_open },
        },
        Mark::NonePlaced => LinePoint::NonePlaced,
        Mark::NoInterval => LinePoint::NoInterval,
    }
}

/// The slots the lines mode evaluates for the visible columns `painted`: one neighbour each side, so a segment to
/// an off-screen point is still drawn.  At most painted.len() + 2 slots.
pub fn line_range(painted: &Range<usize>, n: usize) -> Range<usize> {
    if painted.is_empty() {
        return painted.clone();
    }
    painted.start.saturating_sub(1)..(painted.end + 1).min(n)
}

/// Splits (slot, y) values into unbroken runs of CONSECUTIVE slots: a slot without a value ends the run, and so does a
/// jump in slot index.  A run of one point is drawn as a marker.
pub fn runs(points: impl IntoIterator<Item = (usize, Option<f64>)>) -> Vec<Vec<(usize, f64)>> {
    let mut out: Vec<Vec<(usize, f64)>> = Vec::new();
    let mut cur: Vec<(usize, f64)> = Vec::new();
    for (i, y) in points {
        match y {
            Some(y) => {
                if cur.last().is_some_and(|&(j, _)| j + 1 != i) {
                    out.push(std::mem::take(&mut cur));
                }
                cur.push((i, y));
            }
            None => {
                if !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
            }
        }
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// The trailing moving average over `range` with window `n` (clamped to 1..=SMOOTH_MAX), for pixels only.
///
/// At slot i the result is the mean of the values of slots max(i - n + 1, run start)..=i, where the run is the
/// unbroken stretch of slots with a value that ends at i: the window is TRUNCATED at a break and at the first slot,
/// never padded and never carried across a gap.  A slot without a value has no smoothed value either, so the
/// smoothed line breaks exactly where the raw one does.  `value` is called once per slot of
/// range.start - (n - 1) .. range.end, so the work is range.len() + n - 1 calls, whatever the capture size.
/// Returns, per slot of `range`, (mean, samples in the window).
pub fn smooth(range: Range<usize>, n: usize, mut value: impl FnMut(usize) -> Option<f64>) -> Vec<Option<(f64, usize)>> {
    let n = n.clamp(1, SMOOTH_MAX);
    let start = range.start.saturating_sub(n - 1);
    let mut win: VecDeque<f64> = VecDeque::with_capacity(n);
    let mut out = Vec::with_capacity(range.len());
    for i in start..range.end {
        let v = value(i);
        match v {
            Some(v) => {
                if win.len() == n {
                    win.pop_front();
                }
                win.push_back(v);
            }
            None => win.clear(),
        }
        if i >= range.start {
            // Recomputed from the window (at most SMOOTH_MAX values): no running-sum drift.
            out.push(v.map(|_| (win.iter().sum::<f64>() / win.len() as f64, win.len())));
        }
    }
    out
}

/// The shared y top of the lines mode, in ticks: the largest PPU measured value, the lanes' strip top (every finite
/// lo and hi, and the budget), and the budget line; at least 1 tick.  O(1) from cached maxima.
pub fn lines_top(o: &Owner, lanes_top: Option<f64>, budget_ticks: Option<f64>) -> f64 {
    let ppu = o.measured_sorted().last().map_or(0.0, |&d| d as f64);
    let t = ppu.max(lanes_top.unwrap_or(0.0)).max(budget_ticks.unwrap_or(0.0));
    if t.is_finite() && t >= 1.0 {
        t
    } else {
        1.0
    }
}

/// The y (or x) top of the lines and distribution modes.  `full`: [`lines_top`] (every value fits).  Otherwise the
/// p99 fit: the PPU p99 (the maximum below 20 measured frames), each lane's p99 upper bound (its maximum below 20
/// bars; an infinite one counts its lower end), and the budget, times 1.1.  A lane whose distribution was refused
/// contributes `lanes_top` (every finite end).  A single long frame (a load hitch) then no longer flattens every other
/// line; values above the top are drawn AT the top with an arrow mark, never dropped.  O(lanes) from the cached
/// distribution.
pub fn fit_top(
    o: &Owner,
    lanes: &[Result<LaneDist, String>],
    lanes_top: Option<f64>,
    budget_ticks: Option<f64>,
    full: bool,
) -> f64 {
    if full {
        return lines_top(o, lanes_top, budget_ticks);
    }
    let ppu = match o.percentile_of(99) {
        ps3dk_prof::Metric::Value(v) => Some(v),
        _ => o.measured_sorted().last().copied(),
    }
    .map_or(0.0, |v| v as f64);
    let mut t = ppu.max(budget_ticks.unwrap_or(0.0));
    for l in lanes {
        let Ok(d) = l else {
            t = t.max(lanes_top.unwrap_or(0.0));
            continue;
        };
        let row = d.rows.iter().find(|r| r.label == "p99" && r.value.is_ok()).or(d.rows.iter().find(|r| r.label == "max"));
        if let Some(Ok(b)) = row.map(|r| &r.value) {
            t = t.max(b.hi.unwrap_or(b.lo));
        }
    }
    let t = t * 1.1;
    if t.is_finite() && t >= 1.0 {
        t
    } else {
        1.0
    }
}

/// The value-axis caption of the lines and distribution modes, for an axis top printed as `top`.
pub fn scale_caption(full: bool, top: &str, fallback: usize) -> String {
    if !full && fallback > 0 {
        return format!(
            "top {top} (Fit p99, except {fallback} lane(s) whose distribution was refused: those contribute their \
             finite maximum instead, a conservative fallback; larger values, +inf upper bounds and values not \
             representable as f64 are drawn at the edge with an arrow)"
        );
    }
    if full {
        format!(
            "top {top} (Max: every finite value fits; +inf upper bounds and values not representable as f64 are drawn \
             at the edge with an arrow)"
        )
    } else {
        format!(
            "top {top} (Fit p99: larger values, +inf upper bounds and values not representable as f64 are drawn at the \
             edge with an arrow)"
        )
    }
}

/// The lines shown when the Fit p99 scale falls back for refused lane distributions: one per refused lane, with its
/// reason (empty under Max, or when every lane's distribution was built).
pub fn fit_fallback_notes(labels: &[&str], dist: &[Result<LaneDist, String>], full: bool) -> Vec<String> {
    if full {
        return Vec::new();
    }
    labels
        .iter()
        .zip(dist)
        .filter_map(|(l, d)| d.as_ref().err().map(|why| format!("{l}: {why}; the Fit p99 scale uses this lane's finite maximum (conservative fallback)")))
        .collect()
}

/// An exact integer histogram of the MEASURED values: bin j counts values in [j * width, (j + 1) * width).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hist {
    /// u128: one bin over 0..=u64::MAX is 2^64 wide.
    pub width: u128,
    pub counts: Vec<u64>,
}

impl Hist {
    pub fn total(&self) -> u64 {
        self.counts.iter().sum()
    }
}

/// The histogram of `sorted` (ascending) in at most `bins` bins of one integer width covering 0..=max; None without
/// values or bins.  O(bins log n): one partition_point per bin edge, never a scan of the values.
pub fn histogram(sorted: &[u64], bins: usize) -> Option<Hist> {
    let max = *sorted.last()?;
    if bins == 0 {
        return None;
    }
    let span = u128::from(max) + 1;
    let width = span.div_ceil(bins as u128).max(1);
    let n = span.div_ceil(width) as usize;
    let mut counts = Vec::with_capacity(n);
    let mut below = 0usize;
    for j in 1..=n {
        let edge = width * j as u128;
        let upto = sorted.partition_point(|&d| u128::from(d) < edge);
        counts.push((upto - below) as u64);
        below = upto;
    }
    Some(Hist { width, counts })
}

/// The percentiles shown in distribution mode (100 is the maximum).
pub const PERCENTILES: [u64; 3] = [50, 90, 99];

/// One verdict-map cell: the existing verdict, or the slot kind when there is none.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Cell {
    Over,
    Within,
    Undetermined,
    /// A value without a verdict (no budget selected, or no clock).
    NoVerdict,
    /// The exact comparison was refused (verdict unavailable).
    Unavailable,
    NonePlaced,
    Unknown,
    Refused,
    NoInterval,
}

/// The PPU cell of owner slot `i`: OVER iff elapsed * fps > freq (exact), else WITHIN (a measured 0 is WITHIN).
pub fn ppu_cell(o: &Owner, i: usize, freq_hz: Option<u64>, budget: Budget) -> Cell {
    match o.slots.get(i).map(|s| &s.kind) {
        Some(SlotKind::Measured(d)) => match (budget.fps(), freq_hz) {
            (Some(fps), Some(f)) => {
                if over_budget(*d, fps, f) {
                    Cell::Over
                } else {
                    Cell::Within
                }
            }
            _ => Cell::NoVerdict,
        },
        Some(SlotKind::Unknown(_)) => Cell::Unknown,
        Some(SlotKind::Refused(_)) => Cell::Refused,
        None => Cell::NoInterval,
    }
}

/// The lane cell of owner slot `i`: the bar's cached verdict (computed on the exact bounds).
pub fn lane_cell(o: &Owner, lane: &LaneView, i: usize) -> Cell {
    match mark(o, lane, i) {
        Mark::Bar(b) => match &b.verdict {
            VerdictView::Over => Cell::Over,
            VerdictView::Within => Cell::Within,
            VerdictView::Undetermined => Cell::Undetermined,
            VerdictView::NoBudget | VerdictView::NoClock => Cell::NoVerdict,
            VerdictView::Unavailable(_) => Cell::Unavailable,
        },
        Mark::NonePlaced => Cell::NonePlaced,
        Mark::NoInterval => Cell::NoInterval,
    }
}

/// How a lane series is told apart without relying on hue alone: a colour from a colour-blind-safe set and a dash
/// pattern, cycled together.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dash {
    Solid,
    Dashed,
    Dotted,
}

/// The number of lane colours (see `frames_ui::LANE_COLOURS`).
pub const LANE_COLOURS: usize = 3;

/// Lane `j`'s (colour index, dash): colours cycle first, then the dash pattern, so the first nine lanes are distinct.
pub fn lane_style(j: usize) -> (usize, Dash) {
    let dash = match (j / LANE_COLOURS) % 3 {
        0 => Dash::Solid,
        1 => Dash::Dashed,
        _ => Dash::Dotted,
    };
    (j % LANE_COLOURS, dash)
}

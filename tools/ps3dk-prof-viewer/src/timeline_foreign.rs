//! Timeline rows for SPU lanes and RSX domains (director 2026-11-04 19:32; design timeline-foreign-rows r1 + r2
//! delta + review carries 19:56 and 20:21).  No egui types.
//!
//! A bar is a completed pair whose two endpoints are clock-mapping BOUNDS on the PPU timebase.  It is drawn as a
//! solid core where the pair surely ran (only when EXACT finite `begin.hi < end.lo`; core pixel edges rounded
//! inward) and whiskers out to `begin.lo` / `end.hi` (rounded outward).  An unbounded side runs to the chart edge
//! with an arrow; an open FINITE end is finite (no arrow).  Values reach f64 only after the integer origin is
//! subtracted exactly, so huge ticks with tiny deltas keep their precision.  An EXTRAPOLATED bar is a conditional
//! prediction: every representation of it (core, whiskers, tick, dense run) carries that style and its hover never
//! says it surely ran.
//!
//! **Building** a track is metered ([`ViewBudgets::build_ops`], charged before the work): per bar 6 (the copy and
//! its four endpoint conversions and the core test), the gap pass ([`track_gaps`]: per bar 1 plus [`GAP_MAX`] for
//! its output, charged before it runs), the tile table, and one unit per tile entry (at most
//! `WIDE_TILES` per bar; a wider or unbounded bar is one wide-list entry).  A refusal refuses that track's drawing.
//!
//! **Queries** are bounded per frame: every tile entry and wide-list entry inspected counts against `inspect_cap`
//! BEFORE it is examined (duplicates included), and every emitted primitive (a bar, a tick, a dense run) counts
//! against `paint_cap`.  When a query stops it says so: an exact omitted count only when every candidate was
//! inspected, otherwise "count unknown".

use std::cmp::Ordering;
use std::sync::atomic::{AtomicU64, Ordering as AtOrd};
use std::sync::{Arc, Mutex};

use num_bigint::BigInt;
use num_traits::ToPrimitive;
use ps3dk_prof::clockmap::{Budgets, End, Premises, Rat};
use ps3dk_prof::durations::DurationBudgets;
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_timeline::{
    foreign_timeline_of, has_core, FtBar, FtCounts, FtEpoch, FtSource, FtSourceState, ForeignTimeline, TimelineBudgets,
};
use ps3dk_prof::frames_foreign::Retention;
use ps3dk_prof::Analysis;

use crate::foreign::{ms_exact, ticks_exact, Labels};
use crate::timeline::{Timeline, View};

/// Tiles per track over its finite extent.
pub const TILES: usize = 1024;
/// A bar spanning more tiles than this is kept once in the track's wide list instead.
pub const WIDE_TILES: usize = 8;

/// Per-frame query caps (per track).  Measured 2026-11-04 (release, C:/ps3tc/p33/ft-r1/measure.log): a full view of
/// 10^6 bars stops at the 200 000-inspection cap in 4.9 ms; a 1000-bar zoom inspects ~2000 candidates in 0.05 ms.
/// `paint_cap` bounds every emitted primitive (bars, ticks and dense runs alike).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct QueryCaps {
    pub inspect_cap: usize,
    pub paint_cap: usize,
}

impl Default for QueryCaps {
    fn default() -> Self {
        QueryCaps { inspect_cap: 200_000, paint_cap: 20_000 }
    }
}

/// The view-side build budget.  At the reader's retained-bar cap (2^20) a track needs at most
/// 2^20 * (6 + WIDE_TILES) + TILES = 14.7M units, under the default 2^24.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ViewBudgets {
    pub build_ops: u64,
}

impl Default for ViewBudgets {
    fn default() -> Self {
        ViewBudgets { build_ops: 1 << 24 }
    }
}

/// An endpoint value relative to the timeline origin.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Rel {
    NegInf,
    PosInf,
    /// Finite, relative to the origin (exact subtraction, then f64).
    At(f64),
}

impl Rel {
    fn f(self) -> Option<f64> {
        match self {
            Rel::At(v) => Some(v),
            _ => None,
        }
    }
}

/// `r - origin` as f64, the integer origin removed exactly first.
pub fn rel_of(r: &Rat, origin: u64) -> f64 {
    let num: BigInt = &r.num - BigInt::from(origin) * &r.den;
    let (n, d) = (num.to_f64().unwrap_or(f64::NAN), r.den.to_f64().unwrap_or(f64::NAN));
    n / d
}

fn rel_end(e: &End, origin: u64) -> Rel {
    match e {
        End::NegInf => Rel::NegInf,
        End::PosInf => Rel::PosInf,
        End::At { value, .. } => Rel::At(rel_of(value, origin)),
    }
}

/// One bar, prepared for drawing.
#[derive(Debug, Clone, PartialEq)]
pub struct FBar {
    pub bar: FtBar,
    pub b_lo: Rel,
    pub b_hi: Rel,
    pub e_lo: Rel,
    pub e_hi: Rel,
    /// Exact: finite `begin.hi < end.lo`.
    pub core: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FTrack {
    pub source: SourceKey,
    pub label: String,
    /// The counts line, each count in its own unit and scope.
    pub header: String,
    /// One line per epoch that placed no bars (coverage text, nothing on the axis).
    pub coverage: Vec<String>,
    /// The source (reader) or its drawing (view build budget) refused, or None.
    pub refusal: Option<String>,
    /// Sorted as the reader sorted them (by begin.lo, begin.hi, epoch): an index is a stable identity.
    pub bars: Vec<FBar>,
    pub max_depth: u8,
    /// Stretches (relative, ascending, disjoint) of the gap axis that NO placed completed pair's outer extent
    /// [begin.lo, end.hi] meets, each longer than 1/GAP_FRACTION of that axis ([`track_gaps`]).  The gap axis is the
    /// PPU axis in [`build`], else the union of the bars' finite endpoints.  It says nothing about work that was not
    /// placed: [`FTrack::unplaced`] lists that coverage.
    pub gaps: Vec<(f64, f64)>,
    /// The coverage the gaps cannot speak for: pairs not placed, censored, unpaired, open, loss records and epochs
    /// without an inventory (from the reader's counts; "unknown" where the reader does not know).
    pub unplaced: String,
    /// An SPU lane of a file with order records: its ticks rows (the bars come from causal placement, not from
    /// the clock mapping).  None for every other track.
    pub spu: Option<crate::timeline_spu::SpuTrackInfo>,
    tiles: Vec<Vec<u32>>,
    wide: Vec<u32>,
    /// The finite extent the tiles cover (relative), or None when no bar has a finite outer extent.
    span: Option<(f64, f64)>,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct FTimeline {
    /// SPU lanes first, then RSX domains, each in source order.
    pub tracks: Vec<FTrack>,
    pub notice: Option<String>,
    pub caption: String,
    pub w32: bool,
    /// The integer origin every `Rel` is relative to.
    pub origin: u64,
    /// Set only for a capture with no PPU data: the axis to use (see [`foreign_extent`]).
    pub extent: Option<(u64, u64)>,
    /// The finite bounds of the INTERPOLATED bars, relative to `origin` (for "Fit all"); None when there are none.
    pub interp_extent: Option<(f64, f64)>,
}

pub fn source_label(k: SourceKey) -> String {
    match k {
        SourceKey::Lane(id) => format!("SPU lane {id}"),
        SourceKey::Gpu(id) => format!("RSX domain {id}"),
    }
}

fn epoch_line(epoch: u32, s: &FtEpoch, pairs: Option<u64>) -> Option<String> {
    let p = pairs.map_or("pairs unknown".to_string(), |n| format!("{n} pairs not placed"));
    let why = match s {
        FtEpoch::Placed { .. } => return None,
        FtEpoch::AmbiguousLift => "SPU clock lift ambiguous (turn on W32 to assume no wrap)".to_string(),
        FtEpoch::ClockUnsupported(l) => format!("clock mapping UNSUPPORTED ({})", l.name()),
        FtEpoch::NoObservations => "no clock observations".to_string(),
        FtEpoch::Inconsistent => "clock observations inconsistent".to_string(),
        FtEpoch::DurationUnsupported(l) => format!("durations UNSUPPORTED ({})", l.name()),
        FtEpoch::OrderContradicted => "ORDER-CONTRADICTED".to_string(),
        FtEpoch::Refused(l) => format!("timeline REFUSED ({})", l.name()),
        FtEpoch::NotComputed(l) => format!("not computed ({})", l.name()),
    };
    Some(format!("epoch {epoch}: {why}; {p}"))
}

fn header(s: &FtSource) -> String {
    let c = &s.counts;
    let mut parts = vec![format!("pairs placed {}", c.placed)];
    if c.extrapolated > 0 {
        parts.push(format!("of them extrapolated {}", c.extrapolated));
    }
    if c.too_deep > 0 {
        parts.push(format!("deeper than the row cap {}", c.too_deep));
    }
    if c.not_placed > 0 {
        parts.push(format!("pairs not placed {}", c.not_placed));
    }
    if c.inventory_epochs > 0 {
        let open = c.open.map_or("unknown".to_string(), |o| o.to_string());
        let loss = c.loss_records.map_or("unknown".to_string(), |o| o.to_string());
        parts.push(format!(
            "events in {} epoch(s) with an inventory: open {open}, censored {}, unpaired {}; loss records {loss} (pairs unknown)",
            c.inventory_epochs, c.censored, c.unpaired
        ));
    }
    if c.no_inventory_epochs > 0 {
        parts.push(format!("{} epoch(s) without an inventory: their events are unknown", c.no_inventory_epochs));
    }
    parts.join("; ")
}

fn tile_of(v: f64, lo: f64, hi: f64) -> usize {
    if hi <= lo {
        return 0;
    }
    (((v - lo) / (hi - lo) * TILES as f64).floor().max(0.0) as usize).min(TILES - 1)
}

/// The outer extent of a bar (relative), None on an unbounded side.
fn outer(b: &FBar) -> Option<(f64, f64)> {
    Some((b.b_lo.f()?, b.e_hi.f()?))
}

fn new_track(s: &FtSource) -> FTrack {
    FTrack {
        source: s.source,
        label: source_label(s.source),
        header: header(s),
        coverage: s.epochs.iter().filter_map(|r| epoch_line(r.epoch, &r.state, r.pairs)).collect(),
        refusal: None,
        bars: Vec::new(),
        max_depth: 0,
        gaps: Vec::new(),
        unplaced: unplaced_text(&s.counts),
        spu: None,
        tiles: Vec::new(),
        wide: Vec::new(),
        span: None,
    }
}

/// One source's row, metered by `vb` (exposed for tests: a synthetic source exercises the geometry directly).  The
/// gaps are taken over the bars' own finite extent (no PPU axis).
pub fn build_track_with(s: &FtSource, origin: u64, vb: ViewBudgets) -> FTrack {
    build_track_axis(s, origin, None, vb)
}

/// As [`build_track_with`], with the gaps taken over the PPU axis [0, axis_end] when it is given.
pub fn build_track_axis(s: &FtSource, origin: u64, axis_end: Option<f64>, vb: ViewBudgets) -> FTrack {
    let mut t = new_track(s);
    let bars = match &s.state {
        FtSourceState::Refused(l) => {
            t.refusal = Some(format!("{}: timeline REFUSED ({})", t.label, l.name()));
            return t;
        }
        FtSourceState::Bars(v) => v,
    };
    let mut used: u64 = 0;
    let mut charge = |n: u64| -> bool {
        used = used.saturating_add(n);
        used <= vb.build_ops
    };
    let refuse = |mut t: FTrack| {
        t.refusal = Some(format!("{}: drawing REFUSED (view build budget)", t.label));
        t.bars = Vec::new();
        t.tiles = Vec::new();
        t.wide = Vec::new();
        t.gaps = Vec::new();
        t.span = None;
        t
    };
    if !charge(6 * bars.len() as u64) {
        return refuse(t);
    }
    t.bars = bars
        .iter()
        .map(|b| FBar {
            b_lo: rel_end(&b.begin.lo, origin),
            b_hi: rel_end(&b.begin.hi, origin),
            e_lo: rel_end(&b.end.lo, origin),
            e_hi: rel_end(&b.end.hi, origin),
            core: has_core(b),
            bar: b.clone(),
        })
        .collect();
    t.max_depth = t.bars.iter().map(|b| b.bar.depth).max().unwrap_or(0);
    if !charge(t.bars.len() as u64 + GAP_MAX as u64) {
        return refuse(t);
    }
    t.gaps = track_gaps(&t.bars, axis_end.map(|e| (0.0, e)));
    let (mut lo, mut hi) = (f64::INFINITY, f64::NEG_INFINITY);
    for b in &t.bars {
        if let Some((a, z)) = outer(b) {
            lo = lo.min(a);
            hi = hi.max(z);
        }
    }
    if lo > hi {
        if !charge(t.bars.len() as u64) {
            return refuse(t);
        }
        t.wide = (0..t.bars.len() as u32).collect();
        return t;
    }
    // Count the tile entries first (charged), then allocate and fill.
    let mut entries: u64 = 0;
    for b in &t.bars {
        entries += match outer(b) {
            Some((a, z)) => {
                let span = tile_of(z, lo, hi) - tile_of(a, lo, hi) + 1;
                if span > WIDE_TILES { 1 } else { span as u64 }
            }
            None => 1,
        };
    }
    if !charge(TILES as u64 + entries) {
        return refuse(t);
    }
    t.span = Some((lo, hi));
    t.tiles = vec![Vec::new(); TILES];
    for (i, b) in t.bars.iter().enumerate() {
        match outer(b) {
            Some((a, z)) => {
                let (ta, tz) = (tile_of(a, lo, hi), tile_of(z, lo, hi));
                if tz - ta + 1 > WIDE_TILES {
                    t.wide.push(i as u32);
                } else {
                    for tile in &mut t.tiles[ta..=tz] {
                        tile.push(i as u32);
                    }
                }
            }
            None => t.wide.push(i as u32),
        }
    }
    t
}

/// A gap must be longer than (axis / GAP_FRACTION) to be listed.
pub const GAP_FRACTION: f64 = 50.0;
/// The most gaps one track can list: disjoint stretches each longer than axis / GAP_FRACTION (the allocation the
/// view budget charges up front).
pub const GAP_MAX: usize = GAP_FRACTION as usize;

/// The stretches of the axis [a, b] that no bar's outer extent [begin.lo, end.hi] meets, each longer than
/// (b - a) / GAP_FRACTION, clipped to [a, b].  Every bar counts, including the finite endpoint of a half-unbounded
/// bar: an unbounded begin covers everything before its end, an unbounded end everything after its begin.
///
/// One sweep over `bars` in the reader's order (ascending begin.lo, unbounded first).  Bars out of that order give no
/// gaps at all (never a gap that could cross a bar).  Without an axis, [a, b] is the union of the bars' finite
/// endpoints and no edge stretch is listed.  At most GAP_MAX gaps.
pub fn track_gaps(bars: &[FBar], axis: Option<(f64, f64)>) -> Vec<(f64, f64)> {
    let start = |x: &FBar| x.b_lo.f().unwrap_or(f64::NEG_INFINITY);
    let finish = |x: &FBar| x.e_hi.f().unwrap_or(f64::INFINITY);
    let (a, b, edges) = match axis {
        Some((a, b)) => (a, b, true),
        None => {
            let fin = bars.iter().flat_map(|x| [x.b_lo.f(), x.e_hi.f()]).flatten();
            let (lo, hi) = fin.fold((f64::INFINITY, f64::NEG_INFINITY), |(l, h), v| (l.min(v), h.max(v)));
            (lo, hi, false)
        }
    };
    if !(a.is_finite() && b.is_finite() && b > a) || bars.is_empty() {
        return Vec::new();
    }
    let min = (b - a) / GAP_FRACTION;
    let mut out = Vec::new();
    // `cursor`: everything before it is covered by a bar seen so far (or is before the axis).
    let mut cursor = if edges { a } else { f64::NEG_INFINITY };
    let mut last_start = f64::NEG_INFINITY;
    for x in bars {
        let (s0, e0) = (start(x), finish(x));
        if s0 < last_start || s0.is_nan() || e0.is_nan() {
            return Vec::new();
        }
        last_start = s0;
        if cursor >= b {
            continue; // the rest of the axis is covered; keep checking the order
        }
        let gap_end = s0.min(b);
        if cursor.is_finite() && cursor >= a && gap_end - cursor > min && out.len() < GAP_MAX {
            out.push((cursor, gap_end));
        }
        cursor = cursor.max(e0);
    }
    if edges && cursor.is_finite() && b - cursor.max(a) > min && out.len() < GAP_MAX {
        out.push((cursor.max(a), b));
    }
    out
}

/// The label drawn inside a gap.
pub const GAP_LABEL: &str = "no placed pairs";

fn unplaced_text(c: &FtCounts) -> String {
    let unknown = |v: Option<u64>| v.map_or("unknown".to_string(), |n| n.to_string());
    format!(
        "Not shown on this row at all: {} pairs not placed, {} censored, {} unpaired, {} open, loss records {}, {} \
         epoch(s) without an inventory (their events unknown)",
        c.not_placed,
        c.censored,
        c.unpaired,
        unknown(c.open),
        unknown(c.loss_records),
        c.no_inventory_epochs
    )
}

/// The track's gap lines: the stretches no placed completed-pair outer extent meets, and, separately, the coverage
/// that was not placed (which those stretches cannot speak for).  None without gaps.
pub fn gaps_line(t: &FTrack, tb: crate::model::TimeBase) -> Option<String> {
    let longest = t.gaps.iter().map(|&(a, b)| b - a).fold(0.0_f64, f64::max);
    (!t.gaps.is_empty()).then(|| {
        format!(
            "{} shaded stretch(es) (longest {}): no placed completed pair's outer extent (earliest begin to latest \
             end) meets them; work that was not placed may still fall there. {}",
            t.gaps.len(),
            tb.ticks_f(longest),
            t.unplaced
        )
    })
}

pub fn build_track(s: &FtSource, origin: u64) -> FTrack {
    build_track_with(s, origin, ViewBudgets::default())
}

/// Builds the rows.  The shared PPU-axis rule: `origin` is the timeline origin and `axis_len` the PPU data extent;
/// bars outside it are counted in their track header ("Fit all" widens the axis to the interpolated bounds).
pub fn build(_a: &Analysis, ft: &ForeignTimeline, origin: u64, axis_len: u64, w32: bool) -> FTimeline {
    let axis = Some(axis_len as f64);
    let mut tracks: Vec<FTrack> =
        ft.sources.iter().map(|s| build_track_axis(s, origin, axis, ViewBudgets::default())).collect();
    // SPU lanes first, then RSX domains (stable within each kind).
    tracks.sort_by_key(|t| matches!(t.source, SourceKey::Gpu(_)));
    // Bars wholly outside the shown axis are never drawn there: say how many, so an empty row is explained.
    let end = axis_len as f64;
    let (mut ilo, mut ihi) = (f64::INFINITY, f64::NEG_INFINITY);
    for t in &mut tracks {
        let outside = |b: &&FBar| b.e_hi.f().is_some_and(|v| v < 0.0) || b.b_lo.f().is_some_and(|v| v > end);
        let before = t.bars.iter().filter(|b| b.e_hi.f().is_some_and(|v| v < 0.0)).count();
        let after = t.bars.iter().filter(|b| b.b_lo.f().is_some_and(|v| v > end)).count();
        if before + after > 0 {
            // Fit all widens the axis to INTERPOLATED bounds only: promise only those.
            let interp = t.bars.iter().filter(outside).filter(|b| !b.bar.extrapolated).count();
            let extrap = before + after - interp;
            let mut line = format!("; outside the PPU data range: {before} before, {after} after");
            if interp > 0 {
                line.push_str(&format!(" (Fit all shows the {interp} interpolated)"));
            }
            if extrap > 0 {
                line.push_str(&format!("; {extrap} of them extrapolated (not covered by Fit all)"));
            }
            t.header.push_str(&line);
        }
        for b in t.bars.iter().filter(|b| !b.bar.extrapolated) {
            if let Some((a, z)) = outer(b) {
                ilo = ilo.min(a);
                ihi = ihi.max(z);
            }
        }
    }
    FTimeline {
        tracks,
        notice: ft.retention_refused.then(|| "SPU/RSX rows unavailable: model or pair-fact retention limit".to_string()),
        caption: format!(
            "SPU/RSX bars are completed pairs placed by the clock mapping: solid where the pair surely ran, whiskers \
             where it may have started or ended (premises C, A, S, {}, E). Hatched or dashed: extrapolated (a \
             conditional prediction).",
            if w32 { "W32" } else { "R64" }
        ),
        w32,
        origin,
        extent: None,
        interp_extent: (ilo <= ihi).then_some((ilo, ihi)),
    }
}

/// The finite extent (absolute PPU ticks, floor/ceil) of the interpolated, finite outer bounds of every bar, for a
/// capture with no PPU data: None when no such bound exists (then there is no time axis to draw on).
pub fn foreign_extent(ft: &ForeignTimeline) -> Option<(u64, u64)> {
    let mut lo: Option<Rat> = None;
    let mut hi: Option<Rat> = None;
    let less = |x: &Rat, y: &Rat| ps3dk_prof::foreign_timeline::rat_cmp(x, y) == Ordering::Less;
    for s in &ft.sources {
        if let FtSourceState::Bars(v) = &s.state {
            for b in v.iter().filter(|b| !b.extrapolated) {
                if let (End::At { value: a, .. }, End::At { value: z, .. }) = (&b.begin.lo, &b.end.hi) {
                    if lo.as_ref().is_none_or(|l| less(a, l)) {
                        lo = Some(a.clone());
                    }
                    if hi.as_ref().is_none_or(|h| less(h, z)) {
                        hi = Some(z.clone());
                    }
                }
            }
        }
    }
    let (lo, hi) = (lo?, hi?);
    let l = lo.floor().to_u64().unwrap_or(0);
    let h = hi.ceil().to_u64().unwrap_or(u64::MAX).max(l);
    Some((l, h))
}

// ---- layout --------------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum FDrawKind {
    /// Index into `FTrack::bars`: a bar, or (whisker extent under a pixel) a tick, with its own hover.
    Bar(usize),
    /// Two or more adjacent sub-pixel bars of one style, merged; `count` of them (aggregate hover).
    Dense { count: usize },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FDraw {
    pub depth: u8,
    /// Whisker extent in pixels, rounded outward and clipped to the chart.
    pub w0: f64,
    pub w1: f64,
    /// The core in pixels, rounded inward; None when there is no core or it is under a pixel after rounding.
    pub core: Option<(f64, f64)>,
    /// An unbounded side reaching the chart edge.
    pub arrow_left: bool,
    pub arrow_right: bool,
    pub extrapolated: bool,
    /// The whisker extent is under a pixel: draw a tick.
    pub tick: bool,
    pub kind: FDrawKind,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct FLayout {
    pub draws: Vec<FDraw>,
    /// Candidate entries inspected (tile entries, duplicates included, and wide-list entries).
    pub inspected: usize,
    /// None: everything in view was drawn.  Some(Some(n)): exactly n bars in view were not drawn (the paint cap,
    /// every candidate inspected).  Some(None): the inspection cap stopped the query (count unknown).
    pub omitted: Option<Option<usize>>,
}

fn px(v: Rel, view: &View, width: f64) -> Option<f64> {
    // Multiply before dividing: integer positions over an integer window stay exact.
    v.f().map(|r| (r - view.t0) * width / view.dt)
}

struct Emit {
    draws: Vec<FDraw>,
    emitted: usize,
    cap: usize,
    /// Bars in view that were not emitted (the paint cap).
    lost: usize,
}

impl Emit {
    fn push(&mut self, d: FDraw) {
        if self.emitted < self.cap {
            self.emitted += 1;
            self.draws.push(d);
        } else {
            self.lost += match d.kind {
                FDrawKind::Dense { count } => count,
                FDrawKind::Bar(_) => 1,
            };
        }
    }
}

/// Lays out one track's visible bars, bounded by `caps`.  `show_extrapolated` false hides extrapolated bars (their
/// count stays in the header).
pub fn layout(t: &FTrack, view: &View, width: f64, caps: QueryCaps, show_extrapolated: bool) -> FLayout {
    let (v0, v1) = (view.t0, view.t0 + view.dt);
    let mut em = Emit { draws: Vec::new(), emitted: 0, cap: caps.paint_cap, lost: 0 };
    let mut inspected = 0usize;
    // Pending sub-pixel runs, one per (depth, extrapolated): never mixing styles.
    let mut runs: Vec<[Option<FDraw>; 2]> = vec![[None, None]; t.max_depth as usize + 1];
    let mut stopped = false;
    let visit = |i: usize, em: &mut Emit, runs: &mut Vec<[Option<FDraw>; 2]>| {
        let b = &t.bars[i];
        if b.bar.extrapolated && !show_extrapolated {
            return;
        }
        let lo = b.b_lo.f().unwrap_or(f64::NEG_INFINITY);
        let hi = b.e_hi.f().unwrap_or(f64::INFINITY);
        if hi < v0 || lo > v1 {
            return;
        }
        let raw = match (px(b.b_lo, view, width), px(b.e_hi, view, width)) {
            (Some(x0), Some(x1)) => Some(x1 - x0),
            _ => None,
        };
        let w0 = match b.b_lo {
            Rel::At(_) => px(b.b_lo, view, width).unwrap().floor().max(0.0),
            _ => 0.0,
        };
        let w1 = match b.e_hi {
            Rel::At(_) => px(b.e_hi, view, width).unwrap().ceil().min(width),
            _ => width,
        };
        let core = if b.core {
            let c0 = px(b.b_hi, view, width).expect("finite").ceil().max(0.0);
            let c1 = px(b.e_lo, view, width).expect("finite").floor().min(width);
            (c1 - c0 >= 1.0).then_some((c0, c1))
        } else {
            None
        };
        let sub = raw.is_some_and(|r| r < 1.0);
        let d = FDraw {
            depth: b.bar.depth,
            w0,
            w1,
            core: if sub { None } else { core },
            arrow_left: matches!(b.b_lo, Rel::NegInf),
            arrow_right: matches!(b.e_hi, Rel::PosInf),
            extrapolated: b.bar.extrapolated,
            tick: sub,
            kind: FDrawKind::Bar(i),
        };
        if !sub {
            em.push(d);
            return;
        }
        let slot = &mut runs[d.depth as usize][usize::from(d.extrapolated)];
        match slot {
            // Visit order is not monotone (tiles, then the wide list): merge only intervals that touch within a
            // pixel in EITHER direction, and grow the run both ways.
            Some(cur) if d.w0 <= cur.w1 + 1.0 && d.w1 >= cur.w0 - 1.0 => {
                cur.w0 = cur.w0.min(d.w0);
                cur.w1 = cur.w1.max(d.w1);
                cur.kind = match cur.kind {
                    FDrawKind::Bar(_) => FDrawKind::Dense { count: 2 },
                    FDrawKind::Dense { count } => FDrawKind::Dense { count: count + 1 },
                };
            }
            _ => {
                if let Some(prev) = slot.take() {
                    em.push(prev);
                }
                *slot = Some(d);
            }
        }
    };
    if let Some((lo, hi)) = t.span {
        if v1 >= lo && v0 <= hi {
            let first = tile_of(v0.max(lo), lo, hi);
            let last = tile_of(v1.min(hi), lo, hi);
            'tiles: for tile in first..=last {
                for &i in &t.tiles[tile] {
                    if inspected >= caps.inspect_cap {
                        stopped = true;
                        break 'tiles;
                    }
                    inspected += 1; // counted before the entry is examined, duplicates included
                    let (a, _) = outer(&t.bars[i as usize]).expect("tiled bars are finite");
                    if tile_of(a, lo, hi).max(first) != tile {
                        continue; // seen in an earlier visible tile
                    }
                    visit(i as usize, &mut em, &mut runs);
                }
            }
        }
    }
    if !stopped {
        for &i in &t.wide {
            if inspected >= caps.inspect_cap {
                stopped = true;
                break;
            }
            inspected += 1;
            visit(i as usize, &mut em, &mut runs);
        }
    }
    for pair in runs {
        for d in pair.into_iter().flatten() {
            em.push(d);
        }
    }
    let omitted = if stopped {
        Some(None)
    } else if em.lost > 0 {
        Some(Some(em.lost))
    } else {
        None
    };
    FLayout { draws: em.draws, inspected, omitted }
}

/// The stated omission of a layout: an exact count when every candidate was inspected, else "count unknown".
pub fn omission_text(o: Option<usize>) -> String {
    match o {
        Some(n) => format!("{n} bars not drawn at this zoom; zoom in"),
        None => "additional bars omitted at this zoom (count unknown); zoom in".to_string(),
    }
}

/// What is under the pointer: a bar or tick (its index), or a dense run (count, extrapolated).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FHit {
    Bar(usize),
    Dense { count: usize, extrapolated: bool },
}

/// The primitive under pixel `x` at `depth`, bars and dense runs alike: the LAST drawn one there (drawn on top), so
/// identity follows the paint order under any overlap.
pub fn hit_any(l: &FLayout, depth: u8, x: f64) -> Option<FHit> {
    l.draws.iter().rev().find_map(|d| {
        if d.depth != depth {
            return None;
        }
        match d.kind {
            FDrawKind::Bar(i) if x >= d.w0 - 2.0 && x <= d.w1.max(d.w0 + 1.0) + 2.0 => Some(FHit::Bar(i)),
            FDrawKind::Dense { count } if x >= d.w0 - 1.0 && x <= d.w1 + 1.0 => {
                Some(FHit::Dense { count, extrapolated: d.extrapolated })
            }
            _ => None,
        }
    })
}

/// The bar under pixel `x` at `depth` when the topmost primitive there is a bar (see [`hit_any`]).
pub fn hit(l: &FLayout, depth: u8, x: f64) -> Option<usize> {
    match hit_any(l, depth, x) {
        Some(FHit::Bar(i)) => Some(i),
        _ => None,
    }
}

/// The dense run under pixel `x` at `depth` when the topmost primitive there is a run: (count, extrapolated).
pub fn hit_dense(l: &FLayout, depth: u8, x: f64) -> Option<(usize, bool)> {
    match hit_any(l, depth, x) {
        Some(FHit::Dense { count, extrapolated }) => Some((count, extrapolated)),
        _ => None,
    }
}

fn end_text(e: &End, freq: Option<u64>) -> String {
    match e {
        End::NegInf => "unbounded below".to_string(),
        End::PosInf => "unbounded above".to_string(),
        End::At { value, open } => {
            let t = match freq {
                Some(f) => format!("{} ({})", ticks_exact(value), ms_exact(value, f)),
                None => ticks_exact(value),
            };
            if *open {
                format!("{t}, open (approached, not attained)")
            } else {
                t
            }
        }
    }
}

/// Hover lines: exact values, never the rounded pixel geometry.
pub fn details(a: &Analysis, labels: &Labels, t: &FTrack, i: usize, freq: Option<u64>) -> Vec<String> {
    let b = &t.bars[i];
    let x = &b.bar;
    let mut out = vec![
        labels.zone(a, x.image, x.desc),
        format!("{}, epoch {}, depth {}", t.label, x.epoch, x.depth),
        format!("inclusive {} source ticks", x.inclusive),
        match x.exclusive {
            Some(e) => format!("exclusive {e} source ticks"),
            None => "exclusive: unavailable".to_string(),
        },
        format!("begin in [{}, {}]", end_text(&x.begin.lo, freq), end_text(&x.begin.hi, freq)),
        format!("end in [{}, {}]", end_text(&x.end.lo, freq), end_text(&x.end.hi, freq)),
    ];
    out.push(match (x.extrapolated, b.core) {
        (true, _) => "EXTRAPOLATED: outside the calibration range; these bounds are a conditional model prediction, \
                      not a containment (nothing here surely ran)"
            .to_string(),
        (false, true) => "surely ran between begin's upper and end's lower bound".to_string(),
        (false, false) => "no interval where it surely ran (begin and end bounds touch or overlap)".to_string(),
    });
    out
}

/// The hover line of a dense run.
pub fn dense_text(count: usize, extrapolated: bool) -> String {
    format!(
        "{count} {}pairs narrower than a pixel: zoom in to see them",
        if extrapolated { "EXTRAPOLATED " } else { "" }
    )
}

/// Gives every SPU lane track of an order-record file its ticks info: the header, the refusal and the text of what
/// the row does not draw.  A drawing refusal of the view build budget (already set) is kept.
pub fn attach_spu(v: &mut FTimeline, infos: &mut std::collections::BTreeMap<u32, crate::timeline_spu::SpuTrackInfo>) {
    for t in &mut v.tracks {
        let SourceKey::Lane(id) = t.source else { continue };
        let Some(info) = infos.remove(&id) else { continue };
        let outside = t.header.find("; outside the PPU data range").map(|i| t.header[i..].to_string());
        t.header = info.header.clone();
        if let Some(o) = outside {
            t.header.push_str(&o);
        }
        if t.refusal.is_none() && info.pair_of_bar.is_empty() && info.header.contains("nothing drawn") {
            t.refusal = Some(info.header.clone());
        }
        t.unplaced = format!(
            "Not shown on this row: {} zone(s) with an UNPLACED endpoint (counted, never listed), {} zone(s) not retained",
            info.unplaced, info.not_retained
        );
        t.spu = Some(info);
    }
    v.caption = format!("{} {}", v.caption, crate::timeline_spu::CAPTION);
}

// ---- cache ---------------------------------------------------------------------------------------------------------

/// The Document's one-entry cache, keyed by W32 (a success and a refusal alike).
#[derive(Default)]
pub struct ForeignTimelineSlot {
    entry: Mutex<Option<(bool, Arc<FTimeline>)>>,
    pub builds: AtomicU64,
    /// The SPU ticks analysis (files with order records), built once per Document whatever the W32 choice.
    pub spu: crate::timeline_spu::SpuSlot,
}

impl std::fmt::Debug for ForeignTimelineSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ForeignTimelineSlot").field("builds", &self.builds.load(AtOrd::Relaxed)).finish()
    }
}

impl ForeignTimelineSlot {
    /// The rows for `w32`, built once per (Document, W32): against `tl`'s origin when the capture has PPU data,
    /// else against the foreign extent (which then becomes the axis).
    pub fn view(&self, a: &Analysis, tl: &Timeline, w32: bool) -> Arc<FTimeline> {
        let mut e = self.entry.lock().unwrap_or_else(|p| {
            let mut g = p.into_inner();
            *g = None;
            g
        });
        if let Some((k, v)) = &*e {
            if *k == w32 {
                return Arc::clone(v);
            }
        }
        // Drop the old premise's rows before the new ones exist: no caller can see both.
        *e = None;
        self.builds.fetch_add(1, AtOrd::Relaxed);
        let v = Arc::new(match &a.capture.ext {
            None => FTimeline { w32, origin: tl.start, ..FTimeline::default() },
            Some(x) => {
                let mut ft = foreign_timeline_of(
                    x,
                    Premises { spu_nowrap: w32 },
                    &Budgets::default(),
                    &DurationBudgets::default(),
                    &Retention::default(),
                    &TimelineBudgets::default(),
                );
                // With SPU order records the SPU rows come from the ticks analysis (no W32 needed); every other
                // file keeps the clock-mapping rows unchanged.
                let mut spu_notice = None;
                let infos = if x.order.is_some() {
                    let sv = self.spu.get(a);
                    spu_notice = sv.result.as_ref().and_then(|r| r.exhausted_at.as_ref().map(|e| {
                        let at = e.lane.map_or("the capture".to_string(), |l| format!("lane {l}"));
                        let later = match r.not_listed {
                            Some(n) => format!("{n} later lanes not listed"),
                            None => "later lanes not listed (count unknown)".to_string(),
                        };
                        format!(
                            "SPU ticks analysis: capture budget exhausted at {at} in {}: it and every later SPU lane                              is UNSUPPORTED-RESOURCE; {later}",
                            e.phase
                        )
                    }));
                    let (srcs, infos) = crate::timeline_spu::sources(&sv, &TimelineBudgets::default());
                    ft.sources.retain(|s| !matches!(s.source, SourceKey::Lane(_)));
                    let mut all = srcs;
                    all.append(&mut ft.sources);
                    ft.sources = all;
                    Some(infos)
                } else {
                    None
                };
                let mut v = if tl.has_data {
                    build(a, &ft, tl.start, tl.end - tl.start, w32)
                } else {
                    let extent = foreign_extent(&ft);
                    let (lo, hi) = extent.unwrap_or((0, 0));
                    let mut v = build(a, &ft, lo, hi - lo, w32);
                    v.extent = extent;
                    v
                };
                if let Some(mut infos) = infos {
                    attach_spu(&mut v, &mut infos);
                }
                if let Some(n) = spu_notice {
                    v.notice = Some(match v.notice.take() {
                        Some(o) => format!("{o}; {n}"),
                        None => n,
                    });
                }
                v
            }
        });
        *e = Some((w32, Arc::clone(&v)));
        v
    }
}

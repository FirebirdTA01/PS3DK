//! Timeline view model: lanes, span bars, frame markers, the visible window
//! and its zoom and pan, laid out in pixels with no egui types.
//!
//! Every bar is a span the reader reconstructed. A censored span (open, or
//! truncated at stop) has no end: it is drawn to the edge of the data with an
//! open cap and never given a duration. A span whose END precedes its BEGIN
//! has an unavailable duration: it is drawn as a marker at its BEGIN, never
//! as a zero-length bar.

use crate::model::{TimeBase, ZoneKey};
use ps3dk_prof::Analysis;

/// How a bar ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BarEnd {
    /// END tb, at or after the BEGIN.
    Measured(u64),
    /// Open or truncated at stop: no measured end.
    Censored,
    /// END before BEGIN: the duration is unavailable.
    Unavailable,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bar {
    /// Index into `capture.spans` and `analysis.spans`.
    pub span: usize,
    pub depth: u8,
    pub begin: u64,
    pub end: BarEnd,
    pub key: ZoneKey,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Track {
    pub incarnation: u32,
    pub name: String,
    pub valid: bool,
    pub reasons: String,
    /// Deepest depth present, so the row knows how many bar rows it needs.
    pub max_depth: u8,
    /// Sorted by BEGIN.
    pub bars: Vec<Bar>,
    /// FRAME timebase values, in record order.
    pub frames: Vec<u64>,
    /// The longest measured bar, which bounds the search for visible bars.
    longest: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Timeline {
    pub tracks: Vec<Track>,
    /// The earliest and latest timebase value in the capture: BEGINs, ENDs
    /// and FRAMEs. Censored bars are drawn to `end`.
    pub start: u64,
    pub end: u64,
}

pub fn timeline(a: &Analysis) -> Timeline {
    let cap = &a.capture;
    let mut lo = u64::MAX;
    let mut hi = 0u64;
    let mut seen = |t: u64| {
        lo = lo.min(t);
        hi = hi.max(t);
    };
    let mut tracks = Vec::new();
    for (&inc, lane) in &cap.lanes {
        let view = a.lanes.get(&inc);
        let mut bars = Vec::with_capacity(lane.spans.len());
        let mut longest = 0u64;
        let mut max_depth = 0u8;
        for &idx in &lane.spans {
            let s = &cap.spans[idx];
            seen(s.begin_tb);
            let end = if s.is_censored() {
                // A truncated END still has a timestamp: it extends the
                // data, though the span's duration stays unknown.
                if let Some(e) = s.end_tb {
                    seen(e);
                }
                BarEnd::Censored
            } else {
                match (s.end_tb, s.inclusive()) {
                    (Some(e), Some(len)) => {
                        seen(e);
                        longest = longest.max(len);
                        BarEnd::Measured(e)
                    }
                    (Some(e), None) => {
                        seen(e);
                        BarEnd::Unavailable
                    }
                    (None, _) => BarEnd::Censored,
                }
            };
            let d = cap.descriptors.get(&s.desc);
            max_depth = max_depth.max(s.depth);
            bars.push(Bar {
                span: idx,
                depth: s.depth,
                begin: s.begin_tb,
                end,
                key: (
                    d.map(|d| d.name.clone()).unwrap_or_default(),
                    d.map(|d| d.logical_tag.clone()).unwrap_or_default(),
                ),
            });
        }
        bars.sort_by_key(|b| (b.begin, b.depth));
        let frames: Vec<u64> = lane.frames.iter().map(|f| f.tb).collect();
        for &f in &frames {
            seen(f);
        }
        tracks.push(Track {
            incarnation: inc,
            name: lane
                .info
                .as_ref()
                .map(|t| t.name.clone())
                .unwrap_or_default(),
            valid: view.is_some_and(|v| v.valid),
            reasons: view.map(|v| v.reasons.join("; ")).unwrap_or_default(),
            max_depth,
            bars,
            frames,
            longest,
        });
    }
    if lo > hi {
        lo = 0;
        hi = 0;
    }
    Timeline {
        tracks,
        start: lo,
        end: hi,
    }
}

impl Timeline {
    /// Ticks from the first to the last value, at least 1.
    pub fn length(&self) -> f64 {
        (self.end - self.start).max(1) as f64
    }

    /// A tick relative to `start`. The integer origin is removed before the
    /// conversion to f64, so short intervals keep their precision at any
    /// absolute timebase value; ticks before `start` stay negative.
    pub fn rel(&self, tick: u64) -> f64 {
        (i128::from(tick) - i128::from(self.start)) as f64
    }
}

/// The visible window, in ticks relative to `Timeline::start`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct View {
    pub t0: f64,
    pub dt: f64,
}

/// The narrowest window: a few ticks, so zoom always stops somewhere.
pub const MIN_WINDOW: f64 = 8.0;

impl View {
    pub fn fit(tl: &Timeline) -> View {
        View {
            t0: 0.0,
            dt: tl.length(),
        }
    }

    /// Keep the window inside the data and between `MIN_WINDOW` and the
    /// whole capture.
    pub fn clamp(&mut self, tl: &Timeline) {
        let total = tl.length();
        self.dt = self.dt.clamp(MIN_WINDOW.min(total), total);
        self.t0 = self.t0.clamp(0.0, total - self.dt);
    }

    /// Zoom by `factor` (below 1 zooms in) keeping the tick under `anchor`
    /// (0..1 across the width) in place.
    pub fn zoom(&mut self, tl: &Timeline, anchor: f64, factor: f64) {
        let anchor = anchor.clamp(0.0, 1.0);
        let at = self.t0 + anchor * self.dt;
        self.dt *= factor;
        self.clamp(tl);
        self.t0 = at - anchor * self.dt;
        self.clamp(tl);
    }

    /// Move by a fraction of the width (positive moves later in time).
    pub fn pan(&mut self, tl: &Timeline, fraction: f64) {
        self.t0 += fraction * self.dt;
        self.clamp(tl);
    }

    /// Show exactly the span between two fractions of the width.
    pub fn select(&mut self, tl: &Timeline, a: f64, b: f64) {
        let (a, b) = if a <= b { (a, b) } else { (b, a) };
        let t0 = self.t0 + a.clamp(0.0, 1.0) * self.dt;
        let t1 = self.t0 + b.clamp(0.0, 1.0) * self.dt;
        self.t0 = t0;
        self.dt = t1 - t0;
        self.clamp(tl);
    }

    /// Show `[begin, end]` (absolute ticks) with a margin on both sides.
    pub fn show_range(&mut self, tl: &Timeline, begin: u64, end: u64) {
        let b = tl.rel(begin);
        let e = tl.rel(end);
        let len = (e - b).max(MIN_WINDOW);
        self.t0 = b - len * 0.1;
        self.dt = len * 1.2;
        self.clamp(tl);
    }

    /// x in pixels (0..width) of an absolute tick; may fall outside.
    pub fn x(&self, tl: &Timeline, tick: u64, width: f64) -> f64 {
        (tl.rel(tick) - self.t0) / self.dt * width
    }

    /// The tick offset (relative to `Timeline::start`) at pixel `x`.
    pub fn at(&self, x: f64, width: f64) -> f64 {
        self.t0 + x / width * self.dt
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum DrawKind {
    /// One bar, wide enough to draw and hit: index into `Track::bars`.
    Bar(usize),
    /// Bars narrower than a pixel, merged; `count` of them.
    Dense { count: usize },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Draw {
    pub depth: u8,
    pub x0: f64,
    pub x1: f64,
    pub kind: DrawKind,
    /// The bar's end is open (censored): draw an open cap at `x1`.
    pub open_end: bool,
    /// The bar's duration is unavailable: draw a marker at `x0`.
    pub unavailable: bool,
}

/// A bar narrower than this is merged into its neighbours.
pub const MIN_BAR_PX: f64 = 1.0;

/// Lay out the visible bars of one track across `width` pixels.
pub fn layout(tl: &Timeline, track: &Track, view: &View, width: f64) -> Vec<Draw> {
    // Window edges relative to the timeline start, never absolute f64 ticks.
    let v0 = view.t0;
    let v1 = v0 + view.dt;
    // Measured bars start at most `longest` before the window; censored and
    // unavailable ones are checked from the start (they are rare).
    let from_tick = tl
        .start
        .saturating_add((v0 - track.longest as f64).max(0.0) as u64);
    let first = track.bars.partition_point(|b| b.begin < from_tick);
    let mut out: Vec<Draw> = Vec::new();
    let mut dense: Vec<Option<Draw>> = vec![None; track.max_depth as usize + 1];
    let mut push_dense = |out: &mut Vec<Draw>, d: Draw| {
        let slot = &mut dense[d.depth as usize];
        match slot {
            Some(cur) if d.x0 - cur.x1 <= MIN_BAR_PX => {
                cur.x1 = cur.x1.max(d.x1);
                if let DrawKind::Dense { count } = &mut cur.kind {
                    *count += 1;
                }
            }
            _ => {
                if let Some(prev) = slot.take() {
                    out.push(prev);
                }
                *slot = Some(Draw {
                    kind: DrawKind::Dense { count: 1 },
                    ..d
                });
            }
        }
    };
    let candidates = track.bars[..first]
        .iter()
        .enumerate()
        .filter(|(_, b)| !matches!(b.end, BarEnd::Measured(_)))
        .chain(track.bars.iter().enumerate().skip(first));
    for (i, b) in candidates {
        if tl.rel(b.begin) > v1 {
            break;
        }
        let end_tick = match b.end {
            BarEnd::Measured(e) => e,
            BarEnd::Censored => tl.end,
            BarEnd::Unavailable => b.begin,
        };
        if tl.rel(end_tick) < v0 {
            continue;
        }
        let x0 = view.x(tl, b.begin, width);
        let x1 = view.x(tl, end_tick, width);
        let d = Draw {
            depth: b.depth,
            x0,
            x1,
            kind: DrawKind::Bar(i),
            open_end: b.end == BarEnd::Censored,
            unavailable: b.end == BarEnd::Unavailable,
        };
        if d.unavailable || d.open_end || x1 - x0 >= MIN_BAR_PX {
            out.push(d);
        } else {
            push_dense(&mut out, d);
        }
    }
    out.extend(dense.into_iter().flatten());
    out
}

/// The bar under pixel `x` at `depth`, if a drawn bar covers it. Markers for
/// unavailable durations take a few pixels either side.
pub fn hit(draws: &[Draw], depth: u8, x: f64) -> Option<usize> {
    draws.iter().rev().find_map(|d| match d.kind {
        DrawKind::Bar(i) if d.depth == depth => {
            let (a, b) = if d.unavailable {
                (d.x0 - 3.0, d.x0 + 3.0)
            } else {
                (d.x0, d.x1.max(d.x0 + MIN_BAR_PX))
            };
            (x >= a && x <= b).then_some(i)
        }
        _ => None,
    })
}

/// Frame markers in view, in pixels, dropping any closer than `min_px` to
/// the previous one kept.
pub fn frame_marks(tl: &Timeline, track: &Track, view: &View, width: f64, min_px: f64) -> Vec<f64> {
    let mut out: Vec<f64> = Vec::new();
    for &f in &track.frames {
        let x = view.x(tl, f, width);
        if !(0.0..=width).contains(&x) {
            continue;
        }
        if out.last().is_none_or(|&p| x - p >= min_px) {
            out.push(x);
        }
    }
    out
}

/// Ruler ticks: (x in pixels, label), spaced at least `min_px` apart on a
/// 1-2-5 step, in time when the capture has a clock, else in ticks. Labels
/// are offsets from the start of the capture.
pub fn ruler(view: &View, tb: TimeBase, width: f64, min_px: f64) -> Vec<(f64, String)> {
    let per_px = view.dt / width;
    let unit = tb.freq_hz.map_or(1.0, |f| f as f64 / 1e9); // ticks per ns
    let raw = per_px * min_px / unit; // ns (or ticks) per label at minimum
    let step = nice_step(raw);
    let step_ticks = step * unit;
    let first = (view.t0 / step_ticks).ceil() as i64;
    let scale = ((view.t0 + view.dt) / unit).max(step);
    let mut out = Vec::new();
    let mut k = first;
    loop {
        let t = k as f64 * step_ticks;
        if t > view.t0 + view.dt {
            break;
        }
        let x = (t - view.t0) / per_px;
        let label = match tb.freq_hz {
            Some(_) => format_ns(k as f64 * step, step, scale),
            None => format!("{} ticks", (k as f64 * step).round() as i64),
        };
        out.push((x, label));
        k += 1;
        if out.len() > 1000 {
            break;
        }
    }
    out
}

/// The smallest 1, 2 or 5 times a power of ten that is at least `x`.
pub fn nice_step(x: f64) -> f64 {
    if !x.is_finite() || x <= 0.0 {
        return 1.0;
    }
    let p = 10f64.powf(x.log10().floor());
    for m in [1.0, 2.0, 5.0, 10.0] {
        if m * p >= x {
            return m * p;
        }
    }
    10.0 * p
}

/// "+12.5 ms" style labels: the unit suits `scale_ns` (the largest offset
/// on the ruler, so one ruler uses one unit), with enough decimals for the
/// step.
fn format_ns(ns: f64, step_ns: f64, scale_ns: f64) -> String {
    let (div, unit) = if scale_ns >= 1e9 {
        (1e9, "s")
    } else if scale_ns >= 1e6 {
        (1e6, "ms")
    } else if scale_ns >= 1e3 {
        (1e3, "us")
    } else {
        (1.0, "ns")
    };
    let step = step_ns / div;
    let decimals = if step >= 1.0 {
        0
    } else {
        (-step.log10()).ceil() as usize
    };
    format!("+{:.*} {unit}", decimals, ns / div)
}

/// The tooltip lines for one bar: zone, site, durations as the reader gives
/// them, the frame it is attributed to, and any notes.
pub fn bar_details(a: &Analysis, bar: &Bar, tb: TimeBase) -> Vec<String> {
    let cap = &a.capture;
    let s = &cap.spans[bar.span];
    let mut out = Vec::new();
    let name = if bar.key.1.is_empty() {
        bar.key.0.clone()
    } else {
        format!("{} [{}]", bar.key.0, bar.key.1)
    };
    out.push(name);
    if let Some(d) = cap.descriptors.get(&s.desc) {
        out.push(format!("{}:{}", d.file, d.line));
    }
    if s.is_censored() {
        out.push("Duration unknown: censored (no measured end)".into());
    } else {
        match (s.inclusive(), s.exclusive()) {
            (Some(i), Some(x)) => {
                out.push(format!("Inclusive {}", tb.ticks(i.into())));
                out.push(format!("Exclusive {}", tb.ticks(x.into())));
            }
            (Some(i), None) => {
                out.push(format!("Inclusive {}", tb.ticks(i.into())));
                out.push("Exclusive unavailable (a child's duration is unavailable)".into());
            }
            (None, _) => out.push("Duration unavailable: END before BEGIN".into()),
        }
    }
    if let Some(v) = a.spans.get(bar.span) {
        out.push(format!("Frame: {}", v.attribution));
    }
    let mut notes = Vec::new();
    if s.is_open() {
        notes.push("OPEN");
    }
    if s.truncated_at_stop {
        notes.push("TRUNCATED_AT_STOP");
    }
    if s.affected {
        notes.push("AFFECTED");
    }
    if !notes.is_empty() {
        out.push(notes.join(", "));
    }
    out
}

/// A stable colour slot for a zone, so a zone keeps its colour across
/// lanes, files and sessions.
pub fn color_slot(key: &ZoneKey, slots: usize) -> usize {
    // FNV-1a over name, a separator, and tag.
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in key.0.bytes().chain([0u8]).chain(key.1.bytes()) {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0100_0000_01b3);
    }
    (h % slots.max(1) as u64) as usize
}

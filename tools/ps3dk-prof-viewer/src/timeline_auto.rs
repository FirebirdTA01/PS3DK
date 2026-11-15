//! AUTO Timeline placement. Measurements and clock bounds come from the reader;
//! this module only clips them to the shared viewport and formats annotations.
use crate::{
    auto::AutoView,
    timeline::{Timeline, View},
};
use ps3dk_prof::auto_rsx::{FrameView, FR_TOKEN};

pub const PAINT_CAP: usize = 4096;
pub const PRESENTATION_NOTICE: &str = "Flip-wait and polling positions are not recorded in this capture format. Per-frame aggregates only; polling cadence is observation, not waiting time.";
// TODO (#128, #127): after the reader exposes recorded flip-wait scope begin/end
// ticks, draw those actual intervals; add label-poll aggregate annotations.

#[derive(Debug, Clone, PartialEq)]
pub struct Bar {
    /// Index in the report's frames or the reader's merged busy intervals.
    pub index: usize,
    pub left: f64,
    pub right: f64,
    /// Only the interval inside both conservative endpoint bounds is solid.
    pub core: Option<(f64, f64)>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Layout {
    pub verdict: Vec<Bar>,
    pub busy: Vec<Bar>,
    /// Visible bars omitted by the paint cap, per lane.
    pub omitted: [usize; 2],
    pub unplaced_frames: usize,
}

fn placed(f: &FrameView) -> bool {
    f.flags & FR_TOKEN != 0 && f.tb_end >= f.tb_begin
}

/// Absolute finite PPU extent. No duration slopes or midpoint estimates are used.
pub fn extent(v: &AutoView) -> Option<(u64, u64)> {
    v.report
        .frames
        .iter()
        .filter(|f| placed(f))
        .map(|f| (f.tb_begin, f.tb_end))
        .chain(v.busy.bars.iter().map(|b| (b.begin.0, b.end.1)))
        .fold(None, |acc, (lo, hi)| {
            Some(acc.map_or((lo, hi), |(a, b): (u64, u64)| (a.min(lo), b.max(hi))))
        })
}

pub fn layout(v: &AutoView, tl: &Timeline, view: &View, width: f64) -> Layout {
    let mut out = Layout::default();
    if width <= 0.0 || !width.is_finite() || view.dt <= 0.0 || !view.dt.is_finite() {
        return out;
    }
    // Subtract the integer origin before conversion, preserving tiny deltas at
    // large timestamps. Whiskers round outwards, solid cores inwards.
    let pixel = |t| (tl.rel(t) - view.t0) / view.dt * width;
    let bar = |index, begin: (u64, u64), end: (u64, u64)| {
        let (left, right) = (pixel(begin.0), pixel(end.1));
        if right < 0.0 || left > width {
            return None;
        }
        let (lo, hi) = (
            pixel(begin.1).ceil().max(0.0),
            pixel(end.0).floor().min(width),
        );
        Some(Bar {
            index,
            left: left.floor().max(0.0),
            right: right.ceil().min(width),
            core: (hi > lo).then_some((lo, hi)),
        })
    };
    for (i, f) in v.report.frames.iter().enumerate() {
        if !placed(f) {
            out.unplaced_frames += 1;
            continue;
        }
        if let Some(b) = bar(i, (f.tb_begin, f.tb_begin), (f.tb_end, f.tb_end)) {
            if out.verdict.len() < PAINT_CAP {
                out.verdict.push(b);
            } else {
                out.omitted[0] += 1;
            }
        }
    }
    for (i, b) in v.busy.bars.iter().enumerate() {
        if let Some(b) = bar(i, b.begin, b.end) {
            if out.busy.len() < PAINT_CAP {
                out.busy.push(b);
            } else {
                out.omitted[1] += 1;
            }
        }
    }
    out
}

pub fn presentation_text(f: &FrameView) -> String {
    let Some(p) = &f.presentation else {
        return "No presentation observations".into();
    };
    let mut parts = Vec::new();
    if let Some((ticks, scopes)) = p.flip_wait {
        parts.push(format!(
            "flip-wait: {ticks} ticks / {scopes} scope(s); {} timeout(s), {} spanning",
            p.flip_wait_timeouts, p.flip_wait_spanning
        ));
    }
    if let Some(c) = p.cadence {
        parts.push(format!(
            "polling cadence: {} ticks / {} run(s); {} broken, {} spanning",
            c.ticks, c.runs, c.broken, c.spanning
        ));
    }
    if let Some((count, ticks)) = p.legacy_polls {
        parts.push(format!(
            "poll intervals: {ticks} ticks / {count} interval(s)"
        ));
    }
    parts.join(" | ")
}

//! Frames page view model: per frame owner, one slot per FRAME with the reader's elapsed Metric, an exact FPS
//! budget test, measured-frame statistics, and per-frame hot-zone tables gated on the reader's W.  No egui types.
//!
//! Everything comes from the production reader's `Analysis`; nothing here creates a new measurement:
//! - a slot is MEASURED only when the reader's FrameElapsed is Value (the owner is valid, the next FRAME record is
//!   k + 1 and not earlier); UNKNOWN and REFUSED slots keep the reader's reason and are never 0;
//! - OVER budget is `elapsed * fps > freq_hz` in u128 (a frame exactly at the budget is not over);
//! - a hot-zone table has numeric sums only when the reader's W(owner, k, lane) is Value, and then its total equals
//!   that W exactly; a refused W shows its reasons and span counts, never a number.
//!
//! [`FramesIndex`] is built once per Document; repaints and selections read it and the selected frame's own span
//! list only.

use std::collections::BTreeMap;

use ps3dk_prof::{Analysis, Attribution, Metric};

/// The FPS budgets offered (D4): 30 (the default), 60, or none.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Budget {
    Fps30,
    Fps60,
    None,
}

impl Budget {
    pub const ALL: [Budget; 3] = [Budget::Fps30, Budget::Fps60, Budget::None];
    pub fn fps(self) -> Option<u64> {
        match self {
            Budget::Fps30 => Some(30),
            Budget::Fps60 => Some(60),
            Budget::None => None,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Budget::Fps30 => "30 FPS",
            Budget::Fps60 => "60 FPS",
            Budget::None => "No budget",
        }
    }
}

/// True when `elapsed` ticks exceed the budget of `fps` at `freq_hz`: elapsed * fps > freq_hz, exactly.
pub fn over_budget(elapsed: u64, fps: u64, freq_hz: u64) -> bool {
    u128::from(elapsed) * u128::from(fps) > u128::from(freq_hz)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlotKind {
    /// The reader's elapsed ticks (0 is a valid measured zero).
    Measured(u64),
    Unknown(String),
    Refused(String),
}

/// One FRAME k of an owner.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Slot {
    pub frame: u64,
    pub tb: u64,
    /// The FRAME k+1 record's tb, only when that record is the next one and the slot is MEASURED: the only
    /// interval the Timeline may be framed on.
    pub next_tb: Option<u64>,
    pub kind: SlotKind,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Owner {
    pub incarnation: u32,
    pub name: String,
    pub valid: bool,
    pub reasons: String,
    /// In record order, which is ascending frame id (the reader's FRAME id rule).
    pub slots: Vec<Slot>,
    /// The MEASURED elapsed values, ascending.
    sorted: Vec<u64>,
    /// Built once with the slots: the counts, exact sum and percentiles a repaint must not recompute.
    unknown: u64,
    refused: u64,
    sum: u128,
    p50: Metric<u64>,
    p90: Metric<u64>,
}

impl Owner {
    /// The slot of frame id `k` (binary search: the slots are in ascending frame order).
    pub fn slot_of(&self, k: u64) -> Option<(usize, &Slot)> {
        self.slots.binary_search_by_key(&k, |s| s.frame).ok().map(|i| (i, &self.slots[i]))
    }
}

/// The claimed spans of one (owner, frame, lane) and the reader's W for that key.
#[derive(Debug, Clone, PartialEq)]
struct Group {
    spans: Vec<usize>,
    work: Option<Metric<u128>>,
}

/// Spans of one lane with no frame claim (capture-wide): never an exclusion of any frame.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Unattributed {
    pub pre_frame: u64,
    pub loss: u64,
    pub not_owner: u64,
}

/// The frame facts of one Document, built once.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct FramesIndex {
    pub freq_hz: Option<u64>,
    /// Frame owners (incarnations with FRAME records), ascending.
    pub owners: Vec<Owner>,
    /// Span indices claimed by (owner, frame, lane), in capture order, each with the reader's W.
    claimed: BTreeMap<(u32, u64, u32), Group>,
    /// Per lane, ascending.
    pub unattributed: BTreeMap<u32, Unattributed>,
}

impl FramesIndex {
    pub fn build(a: &Analysis) -> FramesIndex {
        let cap = &a.capture;
        // The reader's Metrics, keyed once (never a per-frame or per-click scan of the reader's lists).
        let elapsed: BTreeMap<(u32, u64), &Metric<u64>> =
            a.frames.iter().map(|e| ((e.owner, e.frame), &e.value)).collect();
        let work: BTreeMap<(u32, u64, u32), &Metric<u128>> =
            a.work.iter().map(|w| ((w.owner, w.frame, w.lane), &w.value)).collect();
        let mut owners = Vec::new();
        for (&inc, lane) in &cap.lanes {
            if lane.frames.is_empty() {
                continue;
            }
            let view = a.lanes.get(&inc);
            let mut slots = Vec::with_capacity(lane.frames.len());
            for (i, fr) in lane.frames.iter().enumerate() {
                let kind = match elapsed.get(&(inc, fr.id)).copied() {
                    Some(Metric::Value(d)) => SlotKind::Measured(*d),
                    Some(Metric::Unknown(why)) => SlotKind::Unknown(why.clone()),
                    Some(Metric::Refused(why)) => SlotKind::Refused(why.clone()),
                    None => SlotKind::Unknown("no elapsed record".to_string()),
                };
                let next_tb = match kind {
                    SlotKind::Measured(_) => lane
                        .frames
                        .get(i + 1)
                        .filter(|n| fr.id.checked_add(1) == Some(n.id) && n.tb >= fr.tb)
                        .map(|n| n.tb),
                    _ => None,
                };
                slots.push(Slot { frame: fr.id, tb: fr.tb, next_tb, kind });
            }
            let mut sorted: Vec<u64> = slots
                .iter()
                .filter_map(|s| match s.kind {
                    SlotKind::Measured(d) => Some(d),
                    _ => None,
                })
                .collect();
            sorted.sort_unstable();
            let unknown = slots.iter().filter(|s| matches!(s.kind, SlotKind::Unknown(_))).count() as u64;
            let refused = slots.iter().filter(|s| matches!(s.kind, SlotKind::Refused(_))).count() as u64;
            owners.push(Owner {
                incarnation: inc,
                name: lane.info.as_ref().map_or(String::new(), |i| i.name.clone()),
                valid: view.is_some_and(|v| v.valid),
                reasons: view.map_or(String::new(), |v| v.reasons.join(", ")),
                unknown,
                refused,
                sum: sorted.iter().map(|&d| u128::from(d)).sum(),
                p50: percentile(&sorted, 50),
                p90: percentile(&sorted, 90),
                slots,
                sorted,
            });
        }
        let mut claimed: BTreeMap<(u32, u64, u32), Group> = BTreeMap::new();
        let mut unattributed: BTreeMap<u32, Unattributed> = BTreeMap::new();
        for (i, (s, v)) in cap.spans.iter().zip(&a.spans).enumerate() {
            match v.attribution.claim() {
                Some((owner, frame)) => {
                    let key = (owner, frame, s.incarnation);
                    claimed
                        .entry(key)
                        .or_insert_with(|| Group { spans: Vec::new(), work: work.get(&key).map(|m| (*m).clone()) })
                        .spans
                        .push(i)
                }
                None => {
                    let u = unattributed.entry(s.incarnation).or_default();
                    match v.attribution {
                        Attribution::PreFrame => u.pre_frame += 1,
                        Attribution::Loss => u.loss += 1,
                        _ => u.not_owner += 1,
                    }
                }
            }
        }
        FramesIndex {
            freq_hz: a.capture.clock.as_ref().map(|c| c.freq_hz).filter(|&f| f > 0),
            owners,
            claimed,
            unattributed,
        }
    }

    pub fn owner(&self, incarnation: u32) -> Option<&Owner> {
        self.owners.binary_search_by_key(&incarnation, |o| o.incarnation).ok().map(|i| &self.owners[i])
    }

    /// The lowest MEASUREMENT VALID owner, else the lowest owner (all REFUSED, reasons visible), else none.
    pub fn default_owner(&self) -> Option<u32> {
        self.owners.iter().find(|o| o.valid).or(self.owners.first()).map(|o| o.incarnation)
    }

    /// The (owner, frame, lane) keys that have claimed spans, i.e. exactly the reader's Work keys.
    pub fn work_keys(&self) -> impl Iterator<Item = &(u32, u64, u32)> {
        self.claimed.keys()
    }

    /// The lanes with attributed work in (owner, frame), ascending.
    pub fn lanes_of(&self, owner: u32, frame: u64) -> Vec<u32> {
        self.claimed.range((owner, frame, 0)..=(owner, frame, u32::MAX)).map(|(&(_, _, l), _)| l).collect()
    }
}

/// Measured-frame statistics of one owner (labels must say "measured frames only").
#[derive(Debug, Clone, PartialEq)]
pub struct Summary {
    pub measured: u64,
    pub unknown: u64,
    pub refused: u64,
    /// Over-budget MEASURED frames for the selected budget; None without a budget, a clock or a measured frame.
    pub over: Option<u64>,
    pub min: Option<u64>,
    pub max: Option<u64>,
    /// Exact sum / count, shown as "mean of N measured frames".
    pub mean: Option<f64>,
    pub p50: Metric<u64>,
    pub p90: Metric<u64>,
}

/// The ZoneStats rank rule: rank = max(1, ceil(p * k / 100)); refused below 20 values.
fn percentile(sorted: &[u64], p: u64) -> Metric<u64> {
    let k = sorted.len() as u64;
    if k < 20 {
        return Metric::Refused(format!("{k} measured frames, percentiles need 20"));
    }
    let rank = (p * k).div_ceil(100).max(1);
    Metric::Value(sorted[(rank - 1) as usize])
}

/// The summary from the owner's cached facts: O(log n) (one partition_point for the over count), never a slot scan.
pub fn summary(o: &Owner, freq_hz: Option<u64>, budget: Budget) -> Summary {
    let n = o.sorted.len();
    let over = match (budget.fps(), freq_hz) {
        (Some(fps), Some(f)) if n > 0 => Some((n - o.sorted.partition_point(|&d| !over_budget(d, fps, f))) as u64),
        _ => None,
    };
    Summary {
        measured: n as u64,
        unknown: o.unknown,
        refused: o.refused,
        over,
        min: o.sorted.first().copied(),
        max: o.sorted.last().copied(),
        mean: (n > 0).then(|| o.sum as f64 / n as f64),
        p50: o.p50.clone(),
        p90: o.p90.clone(),
    }
}

/// Spans of one (owner, frame, lane), each counted once: attribution kind x state.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct KindCounts {
    pub completed: u64,
    pub censored: u64,
    pub unavailable: u64,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SpanCounts {
    pub frame: KindCounts,
    pub owner_invalid: KindCounts,
    pub unresolved: KindCounts,
}

impl SpanCounts {
    pub fn total(&self) -> u64 {
        [self.frame, self.owner_invalid, self.unresolved]
            .iter()
            .map(|k| k.completed + k.censored + k.unavailable)
            .sum()
    }
}

/// One zone identity (a descriptor index) in a W-valid table.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HotRow {
    pub desc: u32,
    /// name [tag] file:line
    pub label: String,
    pub spans: u64,
    pub exclusive: u128,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Table {
    /// W(owner, k, lane) is Value: rows ranked by exact exclusive sum; their total equals `w`.
    Measured { w: u128, rows: Vec<HotRow> },
    /// W is refused: the reasons and the counts only, no number.
    Refused(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneTable {
    pub lane: u32,
    pub table: Table,
    pub counts: SpanCounts,
}

fn zone_label(a: &Analysis, desc: u32) -> String {
    match a.capture.descriptors.get(&desc) {
        Some(d) => {
            let tag = if d.logical_tag.is_empty() { String::new() } else { format!(" [{}]", d.logical_tag) };
            format!("{}{tag} {}:{}", d.name, d.file, d.line)
        }
        None => format!("descriptor {desc} (not published)"),
    }
}

/// The hot-zone tables of (owner, frame): one per lane with a Work entry, from that frame's span lists only.
pub fn frame_tables(a: &Analysis, ix: &FramesIndex, owner: u32, frame: u64) -> Vec<LaneTable> {
    let mut out = Vec::new();
    for ((_, _, lane), group) in ix.claimed.range((owner, frame, 0)..=(owner, frame, u32::MAX)) {
        let idx = &group.spans;
        let mut counts = SpanCounts::default();
        let mut by_desc: BTreeMap<u32, (u64, u128)> = BTreeMap::new();
        for &i in idx {
            let s = &a.capture.spans[i];
            let k = match a.spans[i].attribution {
                Attribution::Frame { .. } => &mut counts.frame,
                Attribution::OwnerInvalid { .. } => &mut counts.owner_invalid,
                _ => &mut counts.unresolved,
            };
            if s.is_censored() {
                k.censored += 1;
            } else {
                match s.exclusive() {
                    Some(x) => {
                        k.completed += 1;
                        let e = by_desc.entry(s.desc).or_default();
                        e.0 += 1;
                        e.1 += u128::from(x);
                    }
                    None => k.unavailable += 1,
                }
            }
        }
        let table = match &group.work {
            Some(Metric::Value(w)) => {
                let mut rows: Vec<HotRow> = by_desc
                    .into_iter()
                    .map(|(desc, (spans, exclusive))| HotRow { desc, label: zone_label(a, desc), spans, exclusive })
                    .collect();
                rows.sort_by(|x, y| y.exclusive.cmp(&x.exclusive).then(x.desc.cmp(&y.desc)));
                let total: u128 = rows.iter().map(|r| r.exclusive).sum();
                if total == *w {
                    Table::Measured { w: *w, rows }
                } else {
                    // Never show numbers that disagree with the reader.
                    Table::Refused(format!("viewer sum {total} differs from the reader's W {w}"))
                }
            }
            Some(Metric::Refused(why)) | Some(Metric::Unknown(why)) => Table::Refused(why.clone()),
            None => Table::Refused("no attributed-work entry".to_string()),
        };
        out.push(LaneTable { lane: *lane, table, counts });
    }
    out
}

/// The Frames page selection: an owner and, optionally, one of its frames.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Selection {
    pub owner: Option<u32>,
    pub frame: Option<u64>,
}

impl Selection {
    /// A fresh document: the default owner, no frame.
    pub fn fresh(ix: &FramesIndex) -> Selection {
        Selection { owner: ix.default_owner(), frame: None }
    }

    /// A newer snapshot of the same capture: keep the owner if it is still present, else the default owner with the
    /// frame cleared (a coincident frame id on another owner is not the same frame); keep the frame only if
    /// (owner, frame) is still present.
    pub fn refreshed(self, ix: &FramesIndex) -> Selection {
        match self.owner.and_then(|o| ix.owner(o)) {
            Some(o) => Selection {
                owner: Some(o.incarnation),
                frame: self.frame.filter(|k| o.slot_of(*k).is_some()),
            },
            None => Selection::fresh(ix),
        }
    }
}

pub const MEASURED_ONLY: &str = "measured frames only";

/// How one slot is drawn: a bar for a positive MEASURED value, otherwise a fixed-size band (MEASURED 0, UNKNOWN and
/// REFUSED are each visible and distinct, never a zero-height bar).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Glyph {
    Bar { height: f64, over: bool },
    Zero,
    Unknown,
    Refused,
}

/// The y scale of one owner's graph for a given budget: built per repaint in O(1) from the cached maximum.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Scale {
    px_per_tick: f64,
    /// (fps, freq_hz) when a budget and a clock exist.
    budget: Option<(u64, u64)>,
    /// The budget line height in pixels.
    pub line: Option<f64>,
}

/// The scale for a graph `height` pixels tall: it covers the largest MEASURED value and the budget line.  Pixels are
/// the only floats; the over test stays the exact integer one.
pub fn scale(o: &Owner, freq_hz: Option<u64>, budget: Budget, height: f64) -> Scale {
    let b = match (budget.fps(), freq_hz) {
        (Some(fps), Some(f)) => Some((fps, f)),
        _ => None,
    };
    let line_ticks = b.map(|(fps, f)| f as f64 / fps as f64);
    let top = o.sorted.last().map_or(0.0, |&d| d as f64).max(line_ticks.unwrap_or(0.0)).max(1.0);
    let px = height / top;
    Scale { px_per_tick: px, budget: b, line: line_ticks.map(|t| t * px) }
}

/// The glyph of slot `i` (one slot: a repaint asks only for the visible ones).
pub fn glyph(o: &Owner, i: usize, sc: &Scale) -> Glyph {
    match o.slots[i].kind {
        SlotKind::Measured(0) => Glyph::Zero,
        SlotKind::Measured(d) => Glyph::Bar {
            height: d as f64 * sc.px_per_tick,
            over: sc.budget.is_some_and(|(fps, f)| over_budget(d, fps, f)),
        },
        SlotKind::Unknown(_) => Glyph::Unknown,
        SlotKind::Refused(_) => Glyph::Refused,
    }
}

/// The slots whose columns intersect the visible span [x0, x1) (pixels from the graph's left edge), for `n` slots of
/// `slot_w` pixels: at most (x1 - x0) / slot_w + 2 slots, whatever `n` is.
pub fn visible_slots(n: usize, slot_w: f64, x0: f64, x1: f64) -> std::ops::Range<usize> {
    if n == 0 || slot_w <= 0.0 || x1 <= x0 {
        return 0..0;
    }
    let first = (x0.max(0.0) / slot_w).floor() as usize;
    let last = (x1.max(0.0) / slot_w).ceil() as usize;
    first.min(n)..last.min(n)
}

/// The absolute slot index at `x` pixels from the graph's left edge, if any.
pub fn slot_at(n: usize, slot_w: f64, x: f64) -> Option<usize> {
    if x < 0.0 || slot_w <= 0.0 {
        return None;
    }
    let i = (x / slot_w).floor() as usize;
    (i < n).then_some(i)
}

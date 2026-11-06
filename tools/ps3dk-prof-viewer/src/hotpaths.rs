//! Hot paths page view model (t_d9789c06 F3a): the reader's [`ps3dk_prof::paths`] result as text rows, built once per
//! Document.  No egui types.  Sums are the reader's exact ticks; the share column is display only (f64).

use ps3dk_prof::paths::{path_text, Counts, Hot, LaneState, PathMap, PathRow};
use ps3dk_prof::{Analysis, Attribution};

use crate::model::TimeBase;

/// Rows shown per lane; the rest are counted.
pub const ROW_LIMIT: usize = 50;
/// Frame links listed per selected row; the rest are counted.
pub const LINK_LIMIT: usize = 20;
/// The row label's leaf name is cut to this many characters (the full chain is shown, wrapped, when selected).
pub const LEAF_CHARS: usize = 80;
pub const CAPTION: &str = "PPU call paths per lane: the descriptor chain along local parents. Sums are exact over \
eligible spans only (not censored, duration available, not affected); lanes are never summed.";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LinkView {
    pub label: String,
    /// Only `Attribution::Frame` navigates: (owner, frame).
    pub nav: Option<(u32, u64)>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct RowView {
    pub rank: usize,
    /// The full chain (root marker and every descriptor), shown wrapped when the row is selected.
    pub text: String,
    /// The row label: the last descriptor's name, at most LEAF_CHARS characters (an ellipsis when cut).
    pub leaf: String,
    pub depth: usize,
    pub hot: bool,
    pub exclusive: String,
    pub inclusive: String,
    /// Exclusive as a share of the lane's retained exclusive total (display only); None when the total is 0.
    pub share: Option<f64>,
    pub counts: String,
    /// None: links dropped by the retention limit.
    pub links: Option<Vec<LinkView>>,
    pub more_links: usize,
}

#[derive(Debug, Clone, PartialEq)]
pub enum LaneKind {
    Refused(String),
    CountsOnly { reasons: String, population: String },
    Paths { lines: Vec<String>, rows: Vec<RowView>, more: Option<String> },
}

#[derive(Debug, Clone, PartialEq)]
pub struct LaneHot {
    pub incarnation: u32,
    pub kind: LaneKind,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct HotView {
    pub lanes: Vec<LaneHot>,
    pub exhausted: Option<String>,
    /// The budget ran out while grouping spans by lane: no lane was formed although the capture has spans.
    pub grouping_refused: Option<String>,
}

/// The page's first message, checked in this order: a grouping refusal (the capture HAS spans), then a capture with
/// no PPU span at all; None when lanes exist.  The page draws exactly this.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Headline {
    Refused(String),
    NoSpans,
}

pub fn headline(hv: &HotView) -> Option<Headline> {
    if let Some(r) = &hv.grouping_refused {
        return Some(Headline::Refused(r.clone()));
    }
    hv.lanes.is_empty().then_some(Headline::NoSpans)
}

pub fn counts_text(c: &Counts) -> String {
    format!(
        "calls {} = censored {} + duration unavailable {} + affected {} + eligible {} (exclusive unavailable {})",
        c.calls, c.censored, c.inclusive_unavailable, c.affected, c.eligible, c.exclusive_unavailable
    )
}

fn link(a: &Analysis, l: &ps3dk_prof::paths::Link) -> LinkView {
    let nav = match l.attribution {
        Attribution::Frame { owner, frame } => Some((owner, frame)),
        _ => None,
    };
    let tb = a.capture.spans.get(l.span).map_or(0, |s| s.begin_tb);
    LinkView { label: format!("span at tb {tb}: {}", l.attribution), nav }
}

fn leaf(a: &Analysis, k: &ps3dk_prof::paths::PathKey) -> String {
    let name = match k.descs.last() {
        Some(d) => a.capture.descriptors.get(d).map_or(format!("desc {d} (not published)"), |x| x.name.clone()),
        None => String::new(),
    };
    let marker = if k.descs.len() == 1 { k.root.text() } else { "" };
    let full = format!("{marker}{name}");
    if full.chars().count() > LEAF_CHARS {
        format!("{}...", full.chars().take(LEAF_CHARS).collect::<String>())
    } else {
        full
    }
}

fn row(a: &Analysis, tb: TimeBase, i: usize, r: &PathRow, total: u128, hot: usize) -> RowView {
    let (links, more_links) = match &r.links {
        Some(l) => (Some(l.iter().take(LINK_LIMIT).map(|x| link(a, x)).collect()), l.len().saturating_sub(LINK_LIMIT)),
        None => (None, 0),
    };
    RowView {
        rank: i + 1,
        text: path_text(a, &r.key),
        leaf: leaf(a, &r.key),
        depth: r.key.descs.len(),
        hot: i < hot,
        exclusive: tb.ticks(r.exclusive),
        inclusive: tb.ticks(r.inclusive),
        share: (total > 0).then(|| r.exclusive as f64 * 100.0 / total as f64),
        counts: counts_text(&r.counts),
        links,
        more_links,
    }
}

pub fn build(a: &Analysis, p: &PathMap) -> HotView {
    let tb = TimeBase::of(a);
    let mut lanes = Vec::with_capacity(p.lanes.len());
    for l in &p.lanes {
        let kind = match &l.state {
            LaneState::Refused(r) => LaneKind::Refused(format!("paths REFUSED ({})", r.name())),
            LaneState::CountsOnly { population, reasons } => {
                LaneKind::CountsOnly { reasons: reasons.clone(), population: counts_text(population) }
            }
            LaneState::Paths { rows, population, too_deep, retained_inclusive, retained_exclusive, hot, links_available } => {
                let k = match hot {
                    Hot::Top(k) => *k,
                    _ => 0,
                };
                let mut lines = vec![
                    format!("lane-wide population: {}", counts_text(population)),
                    format!("deeper than the depth cap: {too_deep} spans (in no path and no total)"),
                    format!(
                        "retained totals (eligible spans in a path): exclusive {}, inclusive {}",
                        tb.ticks(*retained_exclusive),
                        tb.ticks(*retained_inclusive)
                    ),
                    match hot {
                        Hot::Top(k) => format!(
                            "hot: the top {k} path{} hold at least 80% of this lane's RETAINED eligible exclusive time",
                            if *k == 1 { "" } else { "s" }
                        ),
                        Hot::ZeroTotal => "hot: none (the retained exclusive total is 0)".to_string(),
                        Hot::Unavailable => "hot: unavailable (arithmetic)".to_string(),
                    },
                ];
                if !links_available {
                    lines.push("frame links unavailable (retention limit)".to_string());
                }
                let shown: Vec<RowView> =
                    rows.iter().take(ROW_LIMIT).enumerate().map(|(i, r)| row(a, tb, i, r, *retained_exclusive, k)).collect();
                let more = (rows.len() > ROW_LIMIT).then(|| format!("{} more paths (ranked below these)", rows.len() - ROW_LIMIT));
                LaneKind::Paths { lines, rows: shown, more }
            }
        };
        lanes.push(LaneHot { incarnation: l.incarnation, kind });
    }
    HotView {
        lanes,
        exhausted: p.exhausted_at.map(|i| format!("the path budget ran out at lane {i}: it and every later lane are refused")),
        grouping_refused: p
            .grouping_refused
            .then(|| "paths REFUSED: the path budget ran out while grouping spans by lane (no lane has paths)".to_string()),
    }
}

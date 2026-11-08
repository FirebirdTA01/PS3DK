//! Hot paths page, SPU/RSX lanes (t_d9789c06 F3b): the reader's [`ps3dk_prof::foreign_paths`] result as text rows,
//! cached once per (Document, W32).  No egui types.  Bounds are printed outward-rounded with the exact rational on
//! selection (the F2 helpers); shares are bounded and exact, printed outward-rounded to 0.1%.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use num_bigint::BigInt;
use num_integer::Integer;
use ps3dk_prof::clockmap::{map_capture, Budgets, Premises, Rat};
use ps3dk_prof::durations::{durations_pairs, DurationBudgets, PairSink};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_paths::{foreign_paths, FEpochState, FKey, FSourceState, ForeignPathBudgets, ForeignPathMap};
use ps3dk_prof::frames_foreign::Retention;
use ps3dk_prof::Analysis;

use crate::foreign::{bound_exact, bound_rounded, Labels};
use crate::hotpaths::{LEAF_CHARS, ROW_LIMIT};

pub const CAPTION: &str = "paths over completed pairs: the (image, desc) chain along completed parent pairs; an open, \
censored or lost ancestor is marked PARTIAL. Bounds are conditional (premises C, A, S, R64/W32, E); not measurements.";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FRowView {
    pub rank: usize,
    pub leaf: String,
    pub text: String,
    pub depth: usize,
    pub partial: bool,
    pub exclusive: String,
    pub exclusive_exact: String,
    pub inclusive: String,
    pub inclusive_exact: String,
    pub share: String,
    pub order: String,
    pub counts: String,
    pub epochs: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FLaneView {
    pub source: SourceKey,
    pub label: String,
    pub coverage: Vec<String>,
    pub refusal: Option<String>,
    pub lines: Vec<String>,
    pub rows: Vec<FRowView>,
    pub more: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct FHotView {
    pub lanes: Vec<FLaneView>,
    pub notice: Option<String>,
}

fn lane_label(k: SourceKey) -> String {
    match k {
        SourceKey::Lane(id) => format!("SPU lane {id}"),
        SourceKey::Gpu(id) => format!("RSX domain {id}"),
    }
}

/// A share in [0, 1] as a percentage rounded DOWN (lo) or UP (hi) to 0.1%, exact BigInt division.
fn pct(r: &Rat, up: bool) -> String {
    let num = &r.num * BigInt::from(1000u32);
    let (q, rem) = num.div_mod_floor(&r.den);
    let q = if up && rem != BigInt::from(0) { q + 1 } else { q };
    let (i, f) = q.div_rem(&BigInt::from(10u32));
    format!("{i}.{f}%")
}

fn path_text(a: &Analysis, l: &Labels, k: &FKey) -> (String, String) {
    let names: Vec<String> = k.chain.iter().map(|&(img, d)| l.zone(a, img, d)).collect();
    let full = format!("{}{}", k.root.text(), names.join(" > "));
    let last = names.last().cloned().unwrap_or_default();
    let leaf = if last.chars().count() > LEAF_CHARS { format!("{}...", last.chars().take(LEAF_CHARS).collect::<String>()) } else { last };
    (full, leaf)
}

fn epoch_state_text(s: &FEpochState) -> String {
    match s {
        FEpochState::Summed => "summed".to_string(),
        FEpochState::NotMapped(t) => format!("{t}, coverage unknown"),
        FEpochState::DurationUnsupported(t) => format!("durations UNSUPPORTED ({t}), coverage unknown"),
        FEpochState::OrderContradicted => "ORDER-CONTRADICTED, coverage unknown".to_string(),
        FEpochState::Refused(l) => format!("paths REFUSED ({}), coverage unknown", l.name()),
        FEpochState::NotComputed(l) => format!("not computed ({}), coverage unknown", l.name()),
    }
}

pub fn build(a: &Analysis, p: &ForeignPathMap) -> FHotView {
    let freq = a.capture.clock.as_ref().map(|c| c.freq_hz).filter(|&f| f > 0);
    let l = Labels::build(a);
    let notice = p.retention_refused.then(|| "SPU/RSX paths unavailable: pair-fact retention limit".to_string());
    let mut lanes = Vec::with_capacity(p.sources.len());
    for s in &p.sources {
        let coverage = s
            .epochs
            .iter()
            .map(|e| {
                let n = e.pairs.map_or("pairs unknown".to_string(), |n| format!("{n} pairs"));
                format!("epoch {}: {}, {n}", e.epoch, epoch_state_text(&e.state))
            })
            .collect();
        let mut lv = FLaneView { source: s.source, label: lane_label(s.source), coverage, refusal: None, lines: Vec::new(), rows: Vec::new(), more: None };
        match &s.state {
            FSourceState::Refused(r) => lv.refusal = Some(format!("paths REFUSED ({})", r.name())),
            FSourceState::Paths { rows, population, counts, too_deep, total, censored, unpaired, open } => {
                lv.lines = vec![
                    format!(
                        "pairs in summed epochs (population): {} = included {} + extrapolated {} + exclusive unavailable {}",
                        population.pairs, population.included, population.extrapolated, population.exclusive_unavailable
                    ),
                    format!(
                        "kept in paths: {} (included {}, extrapolated {}, exclusive unavailable {}); deeper than the depth \
                         cap: {too_deep} (in the population, in no path and no total)",
                        counts.pairs, counts.included, counts.extrapolated, counts.exclusive_unavailable
                    ),
                    format!("retained included exclusive work (shares are of this): {}", bound_rounded(total, freq)),
                    format!(
                        "non-pair events in summed epochs: censored {censored}, open {}, unpaired {unpaired}",
                        open.map_or("unknown".to_string(), |o| o.to_string())
                    ),
                ];
                for (i, r) in rows.iter().take(ROW_LIMIT).enumerate() {
                    let (text, leaf) = path_text(a, &l, &r.key);
                    let share = match &r.share {
                        Some((lo, hi)) => format!("share [{}, {}] of retained included exclusive work", pct(lo, false), pct(hi, true)),
                        None => "share: none (retained total 0)".to_string(),
                    };
                    let order = match r.separated {
                        Some(true) => "order vs next: separated",
                        Some(false) => "order vs next: not established (bounds overlap)",
                        None => "",
                    };
                    lv.rows.push(FRowView {
                        rank: i + 1,
                        leaf,
                        text,
                        depth: r.key.chain.len(),
                        partial: !r.key.root.complete(),
                        exclusive: bound_rounded(&r.exclusive, freq),
                        exclusive_exact: bound_exact(&r.exclusive, freq),
                        inclusive: bound_rounded(&r.inclusive, freq),
                        inclusive_exact: bound_exact(&r.inclusive, freq),
                        share,
                        order: order.to_string(),
                        counts: format!(
                            "pairs {} = included {} + extrapolated {} + exclusive unavailable {}{}",
                            r.counts.pairs,
                            r.counts.included,
                            r.counts.extrapolated,
                            r.counts.exclusive_unavailable,
                            if r.epochs > 1 { format!("; spans {} epochs (interval sum)", r.epochs) } else { String::new() }
                        ),
                        epochs: r.epochs,
                    });
                }
                lv.more = (rows.len() > ROW_LIMIT).then(|| format!("{} more paths (ranked below these)", rows.len() - ROW_LIMIT));
            }
        }
        lanes.push(lv);
    }
    FHotView { lanes, notice }
}

/// The Document's one-entry cache, keyed by W32 (success and refusal alike).
#[derive(Default)]
pub struct ForeignHotSlot {
    entry: Mutex<Option<(bool, Arc<FHotView>)>>,
    pub builds: AtomicU64,
}

impl std::fmt::Debug for ForeignHotSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ForeignHotSlot").field("builds", &self.builds.load(Ordering::Relaxed)).finish()
    }
}

impl ForeignHotSlot {
    pub fn view(&self, a: &Analysis, w32: bool) -> Arc<FHotView> {
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
        self.builds.fetch_add(1, Ordering::Relaxed);
        let v = Arc::new(match &a.capture.ext {
            None => FHotView::default(),
            Some(x) => {
                let c = map_capture(x, Premises { spu_nowrap: w32 }, &Budgets::default());
                let mut ps = PairSink::new(Retention::default().pair_facts);
                let d = durations_pairs(x, &c, &DurationBudgets::default(), &mut ps);
                build(a, &foreign_paths(&c, &d, &ps, &ForeignPathBudgets::default()))
            }
        });
        *e = Some((w32, Arc::clone(&v)));
        v
    }
}

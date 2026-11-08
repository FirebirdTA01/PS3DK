//! Timeline rows for SPU lanes of a capture WITH SPU order records (file_flags bit 2; spu-ticks slice design 9.4 and
//! delta D1.6).  No egui types.
//!
//! The SPU ticks analysis ([`ps3dk_prof::spu_ticks`]) is built once per Document and key, on first need, in
//! [`SpuSlot`] (a refusal or an exhausted budget is cached as that result and never rebuilt by the viewer).  Each
//! zone pair becomes a bar from its endpoints' placements: begin in [L(BEGIN), U(BEGIN)], end in [L(END), U(END)].
//! FINAL placements are the causal bounds tightened by the RMAX1 assumption; PROVISIONAL ones (a snapshot that is
//! still arriving) are the causal bounds alone.  The solid core is drawn only where U(BEGIN) < L(END), which for a
//! final placement holds only under the RMAX1 assumption; the whiskers reach L(BEGIN) and U(END), and a side without
//! a bound runs to the chart edge as an open arrow.  The causal-only bounds of every pair stay in its details.  A zone with an UNPLACED endpoint is never drawn and never listed: the
//! lane's one status line counts it ([`SpuTrackInfo::header`]).  The per-pair text lives in the bar's tooltip and
//! in the details panel a click opens ([`details`]).  The label is the zone's tick count in SPU decrementer ticks;
//! the bar's length is a placement envelope, never labelled as anything else.
//!
//! A snapshot of a capture that is still arriving is analysed with `Options::provisional` (set by the live
//! client): its lanes without ORDER_FINAL are placed from the order records received so far, and the status line
//! and tooltips say PROVISIONAL.
//!
//! Files without order records keep the clock-mapping rows of [`crate::timeline_foreign`] unchanged.
//!
//! Work bound: the rows of the SPU lanes ([`sources`]) run on one capture-wide meter, `TimelineBudgets::capture_ops`,
//! and each lane is admitted by one charge that covers all of its preparation (its segment scan, its pair scan, the
//! width and bar sorts and the retained bars) BEFORE any of it is done.  A lane the meter cannot pay, and every later
//! lane, is UNSUPPORTED-RESOURCE with nothing drawn and the refusal in its status line; the rows are cached with the
//! timeline, so the refusal is cached too.  [`SpuPrep`] counts the steps where they are done, for the controls.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use ps3dk_prof::clockmap::{End, Projection, Rat};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState, TimelineBudgets};
use ps3dk_prof::spu_ticks::{
    analyze, conversion_sentence, convert, H1Input, LaneOut, LaneState, Options, PairOut, Place, SpuEntry, SpuResult,
    TickState,
};
use ps3dk_prof::Analysis;

/// Every analysis option a cached result depends on: the capture budget, the PROVISIONAL rule, the whole table (each
/// entry's profile id, digests, field rules, premise bits, kinds, evidence, RMAX and ISO annotation, so a same-id
/// entry with changed rules is another key), the W32 premise, the RMAX1 rate, the accumulator limits and the H1
/// input.  `pairs` and `isolation_scale` are not keys: [`SpuSlot::get_with`] always builds with both off.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpuKey {
    pub capture_ops: u64,
    pub provisional: bool,
    pub table: Vec<SpuEntry>,
    pub spu_nowrap: bool,
    pub rmax1: (u128, u128),
    pub seg_acc_max: u128,
    pub htick_acc_max: u128,
    pub h1: Option<H1Input>,
}

impl SpuKey {
    pub fn of(o: &Options) -> SpuKey {
        SpuKey {
            capture_ops: o.capture_ops,
            provisional: o.provisional,
            table: o.table.clone(),
            spu_nowrap: o.spu_nowrap,
            rmax1: o.rmax1,
            seg_acc_max: o.seg_acc_max,
            htick_acc_max: o.htick_acc_max,
            h1: o.h1.clone(),
        }
    }
}

/// The cached analysis of one Document.  The drawing facts (each pair's depth, each lane's image) come with the
/// result, charged to the analysis' own meter: nothing is scanned before or beside it.
#[derive(Debug, Clone)]
pub struct SpuView {
    /// None: the file has no domain extensions.
    pub result: Option<SpuResult>,
    /// The RMAX1 rate (num, den) the result was built with (the assumption the final bounds rest on).
    pub rmax1: (u128, u128),
}

impl SpuView {
    /// The file carries SPU order records (the rows of this module replace the clock-mapping SPU rows).
    pub fn order_present(&self) -> bool {
        self.result.as_ref().is_some_and(|r| r.order_present)
    }
}

/// The Document's SPU ticks cache: one entry, keyed by [`SpuKey`], built on first need.
#[derive(Default)]
pub struct SpuSlot {
    entry: Mutex<Option<(SpuKey, Arc<SpuView>)>>,
    /// How many analyses were built (tests: once per Document and key).
    pub builds: AtomicU64,
    /// The options [`SpuSlot::get`] uses (the reader's defaults; tests may set a table or a budget; the live client
    /// sets `provisional` on a snapshot that is still arriving).
    pub options: Options,
}

impl std::fmt::Debug for SpuSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SpuSlot").field("builds", &self.builds.load(Ordering::Relaxed)).finish()
    }
}

impl SpuSlot {
    /// The analysis with the viewer's options (the reader's defaults: the compiled-in table, the default budget).
    pub fn get(&self, a: &Analysis) -> Arc<SpuView> {
        self.get_with(a, &self.options)
    }

    pub fn with_options(options: Options) -> SpuSlot {
        SpuSlot { options, ..SpuSlot::default() }
    }

    /// The analysis for `opts`, built at most once per key.  A refused or exhausted result is cached as it is.
    pub fn get_with(&self, a: &Analysis, opts: &Options) -> Arc<SpuView> {
        let key = SpuKey::of(opts);
        let mut e = self.entry.lock().unwrap_or_else(|p| {
            let mut g = p.into_inner();
            *g = None;
            g
        });
        if let Some((k, v)) = &*e {
            if *k == key {
                return Arc::clone(v);
            }
        }
        *e = None;
        self.builds.fetch_add(1, Ordering::Relaxed);
        // `--pairs` and the conversion are not needed: every pair is retained; the tooltip converts on demand
        let o = Options { pairs: false, isolation_scale: false, ..opts.clone() };
        let v = Arc::new(SpuView { result: analyze(a, &o), rmax1: o.rmax1 });
        *e = Some((key, Arc::clone(&v)));
        v
    }
}

/// What the SPU row of one lane adds to its `FTrack`.
#[derive(Debug, Clone, PartialEq)]
pub struct SpuTrackInfo {
    pub view: Arc<SpuViewRef>,
    /// Index of the lane in the result.
    pub lane: usize,
    /// Bar index (in the track's bars) -> pair index (in the lane's `pairs_out`).
    pub pair_of_bar: Vec<usize>,
    /// Zones with an UNPLACED endpoint (not drawn, not listed: counted in the status line).
    pub unplaced: u64,
    /// Zones with both endpoints PLACED.
    pub placed: u64,
    /// Every retained eligible zone of the lane.
    pub total: u64,
    /// Lower median of U - L over the PLACED endpoints of the lane (tb ticks), None when there is none.
    pub median_width: Option<u64>,
    /// Drawable zones beyond the retained-bar cap (not drawn, counted).
    pub not_retained: u64,
    /// The placements are PROVISIONAL (a snapshot that is still arriving, no ORDER_FINAL yet).
    pub provisional: bool,
    /// The lane's one compact status line: state, placed/total pairs, median width (or the refusal).
    pub header: String,
    /// What the lane's row preparation charged and counted (the controls).
    pub prep: SpuPrep,
}

/// The row preparation's accounting for one lane: the charge that admitted it (0 when refused) and the steps counted
/// where they were done (0 when refused: nothing is scanned before the charge).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SpuPrep {
    pub charged: u64,
    pub steps: u64,
}

/// The capture-wide meter of the SPU rows: `limit` operations, charged before the work; once a charge fails it and
/// every later lane is refused.
#[derive(Debug, Clone)]
pub struct RowMeter {
    pub limit: u64,
    pub used: u64,
    pub exhausted: bool,
}

impl RowMeter {
    pub fn new(limit: u64) -> RowMeter {
        RowMeter { limit, used: 0, exhausted: false }
    }
    fn charge(&mut self, n: u64) -> bool {
        if self.exhausted {
            return false;
        }
        match self.used.checked_add(n) {
            Some(v) if v <= self.limit => {
                self.used = v;
                true
            }
            _ => {
                self.exhausted = true;
                false
            }
        }
    }
}

/// Fixed charges of one lane's row: its records and status line.
pub const ROW_FIXED_OPS: u64 = 8;
/// Per pair of the scan: the visit, at most two width pushes and one drawable push.
pub const ROW_PAIR_OPS: u64 = 4;
/// Per retained bar: the bar and its pair index.
pub const ROW_BAR_OPS: u64 = 2;

fn clog2(n: u64) -> u64 {
    if n <= 1 {
        0
    } else {
        64 - u64::from((n - 1).leading_zeros())
    }
}

/// The charge of the rows' merge sort on n elements: the scratch buffer (n) and at most n moves per level over
/// ceil(log2 n) levels.
pub fn sort_ops(n: u64) -> u64 {
    n.saturating_mul(clog2(n)).saturating_add(n)
}

/// A stable bottom-up merge sort that counts every scratch slot and move in `steps` (the caller charged
/// [`sort_ops`] first).
fn msort<T: Copy>(v: &mut Vec<T>, steps: &mut u64, less: impl Fn(&T, &T) -> bool) {
    let n = v.len();
    if n < 2 {
        return;
    }
    *steps += n as u64; // the scratch buffer
    let mut src = std::mem::take(v);
    let mut dst: Vec<T> = Vec::with_capacity(n);
    let mut width = 1;
    while width < n {
        dst.clear();
        let mut i = 0;
        while i < n {
            let mid = (i + width).min(n);
            let end = (i + 2 * width).min(n);
            let (mut a, mut b) = (i, mid);
            while a < mid || b < end {
                *steps += 1;
                if b >= end || (a < mid && !less(&src[b], &src[a])) {
                    dst.push(src[a]);
                    a += 1;
                } else {
                    dst.push(src[b]);
                    b += 1;
                }
            }
            i = end;
        }
        std::mem::swap(&mut src, &mut dst);
        width *= 2;
    }
    *v = src;
}

/// The charge that admits one lane's row: everything [`lane_source`] does, bounded from the lane's sizes alone (the
/// segment count, the pair count and the bars it may retain).
pub fn row_ops(segments: u64, pairs: u64, retain: u64) -> u64 {
    let mut c = ROW_FIXED_OPS
        .saturating_add(segments)
        .saturating_add(pairs.saturating_mul(ROW_PAIR_OPS))
        .saturating_add(sort_ops(pairs.saturating_mul(2)));
    if retain > 0 {
        c = c.saturating_add(sort_ops(pairs)).saturating_add(pairs.min(retain).saturating_mul(ROW_BAR_OPS));
    }
    c
}

/// The shared result behind every track (compared by identity, never deep-compared per repaint).
#[derive(Debug)]
pub struct SpuViewRef(pub Arc<SpuView>);

impl PartialEq for SpuViewRef {
    fn eq(&self, o: &SpuViewRef) -> bool {
        Arc::ptr_eq(&self.0, &o.0)
    }
}

impl SpuTrackInfo {
    pub fn lane_out(&self) -> &LaneOut {
        &self.view.0.result.as_ref().expect("a result").lanes[self.lane]
    }
    pub fn pair(&self, bar: usize) -> &PairOut {
        &self.lane_out().pairs_out[self.pair_of_bar[bar]]
    }
}

fn bound(v: Option<u64>, lower: bool) -> End {
    match v {
        Some(t) => End::At { value: Rat::int(i128::from(t)), open: false },
        None if lower => End::NegInf,
        None => End::PosInf,
    }
}

fn lu(p: &Place) -> Option<(Option<u64>, Option<u64>)> {
    match p {
        Place::Placed(l, u) => Some((Some(*l), Some(*u))),
        Place::LowerOnly(l) => Some((Some(*l), None)),
        Place::UpperOnly(u) => Some((None, Some(*u))),
        Place::Unplaced(_) => None,
    }
}

/// The tick label of a pair: "N ticks", or "no tick count (<class>)".
pub fn tick_label(p: &PairOut) -> String {
    match (p.ticks, &p.tick_class) {
        (Some(t), None) => format!("{t} ticks"),
        (Some(t), Some(_)) => format!("{t} ticks (TICKS-CONDITIONAL)"),
        (None, Some(c)) => format!("no tick count ({c})"),
        (None, None) => "no tick count".to_string(),
    }
}

/// The lane state word of the status line: PROVISIONAL, or the ticks state of its segments.
fn state_word(u: &LaneOut) -> String {
    if u.provisional {
        return "PROVISIONAL (no ORDER_FINAL yet)".to_string();
    }
    match &u.state {
        LaneState::Empty => return "EMPTY".to_string(),
        LaneState::EmptyUnproven(_) => return "EMPTY-UNPROVEN".to_string(),
        _ => {}
    }
    let mut unav: Option<&str> = None;
    let mut cond = false;
    for s in &u.segments {
        match &s.state {
            TickState::Unavailable(w) => unav = unav.or(w.first().map(String::as_str)),
            TickState::Conditional(_) => cond = true,
            TickState::Ticks => {}
        }
    }
    match (unav, cond) {
        (Some(w), _) => format!("TICKS-UNAVAILABLE ({w})"),
        (None, true) => "TICKS-CONDITIONAL".to_string(),
        (None, false) if u.segments.is_empty() => "no segments".to_string(),
        (None, false) => "TICKS[profile]".to_string(),
    }
}

/// A drawable zone: pair index, L(BEGIN), U(BEGIN), L(END), U(END).
type Drawable = (usize, Option<u64>, Option<u64>, Option<u64>, Option<u64>);

/// The synthetic source (bars in the timeline order: begin.lo with unbounded first, begin.hi, epoch) and the track
/// info of one lane.  `retain` is the number of bars this lane may still keep under the capture's cap.  The lane is
/// admitted by one charge to `meter` ([`row_ops`]) before any scan, allocation or sort; when the meter cannot pay,
/// nothing of the lane is scanned and its row is UNSUPPORTED-RESOURCE with nothing drawn.
pub fn lane_source(view: &Arc<SpuView>, lane: usize, retain: u64, meter: &mut RowMeter) -> (FtSource, SpuTrackInfo) {
    let r = view.result.as_ref().expect("a result");
    let u = &r.lanes[lane];
    let cost = row_ops(u.segments.len() as u64, u.pairs_out.len() as u64, retain);
    if !meter.charge(cost) {
        let header = format!(
            "SPU lane {}: rows UNSUPPORTED-RESOURCE (SPU row budget: {} of {} operations used, this lane needs {cost}): nothing drawn",
            u.lane_id, meter.used, meter.limit
        );
        let src = FtSource {
            source: SourceKey::Lane(u.lane_id),
            epochs: Vec::new(),
            counts: empty_counts(0),
            state: FtSourceState::Bars(Vec::new()),
        };
        let info = SpuTrackInfo {
            view: Arc::new(SpuViewRef(Arc::clone(view))),
            lane,
            pair_of_bar: Vec::new(),
            unplaced: 0,
            placed: 0,
            total: u.pairs_out.len() as u64,
            median_width: None,
            not_retained: 0,
            provisional: u.provisional,
            header,
            prep: SpuPrep::default(),
        };
        return (src, info);
    }
    let mut steps = 1 + u.segments.len() as u64; // the fixed records and the state word's segment scan
    let mut drawable: Vec<Drawable> = Vec::new();
    let (mut unplaced, mut placed) = (0u64, 0u64);
    let mut widths: Vec<u64> = Vec::new();
    for (i, p) in u.pairs_out.iter().enumerate() {
        steps += 1;
        for pl in [&p.place_b, &p.place_e] {
            if let Place::Placed(l, h) = pl {
                steps += 1;
                widths.push(h - l);
            }
        }
        if matches!((&p.place_b, &p.place_e), (Place::Placed(..), Place::Placed(..))) {
            placed += 1;
        }
        match (lu(&p.place_b), lu(&p.place_e)) {
            (Some((bl, bu)), Some((el, eu))) => {
                steps += 1;
                drawable.push((i, bl, bu, el, eu));
            }
            _ => unplaced += 1,
        }
    }
    msort(&mut widths, &mut steps, |a, b| a < b);
    let median_width = (!widths.is_empty()).then(|| widths[(widths.len() - 1) / 2]);
    let keep = (drawable.len() as u64).min(retain) as usize;
    let not_retained = (drawable.len() - keep) as u64;
    if keep > 0 {
        let key = |x: &Drawable| (x.1.is_some(), x.1.unwrap_or(0), x.2.is_none(), x.2.unwrap_or(0), u.pairs_out[x.0].epoch, x.0);
        msort(&mut drawable, &mut steps, |x, y| key(x) < key(y));
    }
    drawable.truncate(keep);
    let bars: Vec<FtBar> = drawable
        .iter()
        .map(|&(i, bl, bu, el, eu)| {
            steps += ROW_BAR_OPS;
            let p = &u.pairs_out[i];
            FtBar {
                epoch: p.epoch,
                image: u.image,
                desc: p.desc,
                depth: p.depth,
                begin: Projection { lo: bound(bl, true), hi: bound(bu, false), interpolated: true },
                end: Projection { lo: bound(el, true), hi: bound(eu, false), interpolated: true },
                inclusive: p.ticks.map_or(0, |t| u64::try_from(t).unwrap_or(u64::MAX)),
                exclusive: None,
                extrapolated: false,
            }
        })
        .collect();
    let pair_of_bar: Vec<usize> = drawable.iter().map(|d| d.0).collect();
    let total = u.pairs_out.len() as u64;
    let mut header = status_line(&state_word(u), placed, total, unplaced, median_width);
    if not_retained > 0 {
        header.push_str(&format!(
            "; {not_retained} zone{} not retained",
            if not_retained == 1 { "" } else { "s" }
        ));
    }
    let refusal = match &u.state {
        LaneState::Unsupported(w) => {
            Some(format!("SPU lane {}: ticks analysis UNSUPPORTED-RESOURCE ({w}): nothing drawn", u.lane_id))
        }
        LaneState::Refused(w) => Some(format!("SPU lane {}: ticks analysis REFUSED ({w}): nothing drawn", u.lane_id)),
        _ => None,
    };
    let st = FtSourceState::Bars(if refusal.is_some() { Vec::new() } else { bars });
    if let Some(r) = &refusal {
        header = r.clone();
    }
    let n = match &st {
        FtSourceState::Bars(v) => v.len() as u64,
        FtSourceState::Refused(_) => 0,
    };
    let src = FtSource { source: SourceKey::Lane(u.lane_id), epochs: Vec::new(), counts: empty_counts(n), state: st };
    let info = SpuTrackInfo {
        view: Arc::new(SpuViewRef(Arc::clone(view))),
        lane,
        pair_of_bar: if refusal.is_some() { Vec::new() } else { pair_of_bar },
        unplaced,
        placed,
        total,
        median_width,
        not_retained,
        provisional: u.provisional,
        header,
        prep: SpuPrep { charged: cost, steps },
    };
    (src, info)
}

fn empty_counts(placed: u64) -> FtCounts {
    FtCounts {
        placed,
        extrapolated: 0,
        too_deep: 0,
        not_placed: 0,
        open: None,
        censored: 0,
        unpaired: 0,
        loss_records: None,
        inventory_epochs: 0,
        no_inventory_epochs: 0,
    }
}

/// The synthetic sources of every SPU lane of the result, and their track infos (keyed by lane id), under the
/// capture's retained-bar cap and its row meter (`budgets.capture_ops`).
pub fn sources(view: &Arc<SpuView>, budgets: &TimelineBudgets) -> (Vec<FtSource>, BTreeMap<u32, SpuTrackInfo>) {
    let mut out = Vec::new();
    let mut infos = BTreeMap::new();
    let Some(r) = &view.result else { return (out, infos) };
    let mut left = budgets.retained_bars;
    let mut meter = RowMeter::new(budgets.capture_ops);
    for (i, u) in r.lanes.iter().enumerate() {
        let (s, info) = lane_source(view, i, left, &mut meter);
        if let FtSourceState::Bars(b) = &s.state {
            left -= b.len() as u64;
        }
        infos.insert(u.lane_id, info);
        out.push(s);
    }
    (out, infos)
}

/// The lane's status line: "<state> | placed P/T pairs | UNPLACED U | median width W tb".
pub fn status_line(state: &str, placed: u64, total: u64, unplaced: u64, median_width: Option<u64>) -> String {
    let w = median_width.map_or("none".to_string(), |w| format!("{w} tb"));
    format!("{state} | placed {placed}/{total} pairs | UNPLACED {unplaced} | median width {w}")
}

/// Which bounds a placement is (the wording of every SPU placement says which).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Basis {
    /// FINAL: the causal bounds tightened by the RMAX1 assumption.
    Rmax1,
    /// FINAL, but the RMAX1 chain did not run over the endpoint's segment: the causal bounds alone.
    CausalFinal,
    /// PROVISIONAL (a snapshot that is still arriving): the causal bounds alone.
    Provisional,
    /// The causal-only bounds, shown beside a final placement for inspection.
    CausalOnly,
}

impl Basis {
    pub fn text(self) -> &'static str {
        match self {
            Basis::Rmax1 => "final bounds: causal, tightened by the RMAX1 assumption",
            Basis::CausalFinal => "final bounds: causal only, the RMAX1 chain did not apply",
            Basis::Provisional => "PROVISIONAL bounds: causal only, no RMAX1 chain yet",
            Basis::CausalOnly => "causal bounds only, without RMAX1",
        }
    }
    /// The basis of a pair's shown endpoint.
    pub fn of(provisional: bool, rmax1: bool) -> Basis {
        match (provisional, rmax1) {
            (true, _) => Basis::Provisional,
            (false, true) => Basis::Rmax1,
            (false, false) => Basis::CausalFinal,
        }
    }
}

/// Placement text (design 9.3): "placed between L and U (<basis>)".
pub fn place_text(p: &Place, basis: Basis) -> String {
    let b = basis.text();
    match p {
        Place::Placed(l, u) => format!("placed between {l} and {u} ({b})"),
        Place::LowerOnly(l) => format!("LOWER-ONLY: placed after {l}, no upper bound ({b})"),
        Place::UpperOnly(u) => format!("UPPER-ONLY: placed before {u}, no lower bound ({b})"),
        Place::Unplaced(w) => format!("UNPLACED ({w})"),
    }
}

/// The last conversion computed for a tooltip: (track, pair) and its text, with the number of calls made.
#[derive(Debug, Default, Clone)]
pub struct ConvCache {
    pub key: Option<(usize, usize)>,
    pub text: Option<String>,
    pub calls: u64,
}

/// The 9.2 sentence for the hovered pair only (cached by pair index); None unless the option is on, the pair is
/// TICKS[profile] and the matched entry has an ISO annotation.
pub fn conversion_for(cache: &mut ConvCache, track: usize, info: &SpuTrackInfo, bar: usize, on: bool) -> Option<String> {
    if !on {
        return None;
    }
    let r = info.view.0.result.as_ref()?;
    let iso = r.iso.as_ref()?;
    let pi = info.pair_of_bar[bar];
    let p = &info.lane_out().pairs_out[pi];
    if !p.reviewed {
        return None;
    }
    if cache.key != Some((track, pi)) {
        cache.calls += 1;
        let (lo, hi) = convert(p.ticks?, iso, r.freq_hz);
        let rt = |q: (u128, u128)| if q.1 == 1 { q.0.to_string() } else { format!("{}/{}", q.0, q.1) };
        cache.key = Some((track, pi));
        cache.text = Some(conversion_sentence(&rt(iso.lo), &rt(iso.hi), &lo, &hi).trim_start_matches("; ").to_string());
    }
    cache.text.clone()
}

/// Hover lines of an SPU bar: the zone, its tick count, both placements and their widths (uncertainty, not cost).
pub fn details(info: &SpuTrackInfo, zone: &str, bar: usize, freq: Option<u64>) -> Vec<String> {
    let p = info.pair(bar);
    let width = |pl: &Place| match pl {
        Place::Placed(l, u) => match freq {
            Some(f) if f > 0 => format!("{} tb ({:.3} us)", u - l, (u - l) as f64 * 1e6 / f as f64),
            _ => format!("{} tb", u - l),
        },
        _ => "unbounded".to_string(),
    };
    let (bb, be) = (Basis::of(info.provisional, p.rmax1_b), Basis::of(info.provisional, p.rmax1_e));
    let mut v = vec![
        zone.to_string(),
        format!("SPU lane {}, epoch {}, seq {}/{}", info.lane_out().lane_id, p.epoch, p.begin, p.end),
        format!("{} (SPU decrementer ticks)", tick_label(p)),
        format!("BEGIN {}", place_text(&p.place_b, bb)),
        format!("END {}", place_text(&p.place_e, be)),
        format!(
            "placement width BEGIN {}, END {} (uncertainty of the placement, not a cost)",
            width(&p.place_b),
            width(&p.place_e)
        ),
    ];
    // the causal-only bounds stay inspectable beside final bounds that rest on RMAX1
    if bb == Basis::Rmax1 || be == Basis::Rmax1 {
        v.push(format!("BEGIN {}", place_text(&p.causal_b, Basis::CausalOnly)));
        v.push(format!("END {}", place_text(&p.causal_e, Basis::CausalOnly)));
        let (n, d) = info.view.0.rmax1;
        v.push(format!(
            "RMAX1 assumption: the SPU decrementer advances at most {n} tick(s) per {d} tb tick(s); the final bounds \
             and a solid core drawn from them hold only under it"
        ));
    }
    if info.provisional {
        v.push(PROVISIONAL_NOTE.to_string());
    }
    v
}

/// The tooltip and details line of a PROVISIONAL placement.
pub const PROVISIONAL_NOTE: &str = concat!(
    "PROVISIONAL: placed by the causal bounds alone from the order records received so far (no ORDER_FINAL yet, no ",
    "RMAX1 chain); the final placement is decided when the capture ends, adds the RMAX1 assumption, and lies inside ",
    "these bounds unless a later record contradicts them"
);

/// The caption added under the SPU group for a file with order records.
pub const CAPTION: &str = "SPU lanes with order records: each zone is placed on the PPU tb by bounds, never by a clock \
mapping or a fit. FINAL bounds are the causal bounds tightened by the RMAX1 assumption (the SPU decrementer never \
advances faster than the configured rate per tb tick); PROVISIONAL bounds (a capture still arriving) are the causal \
bounds alone. A bar is solid where its BEGIN's upper bound precedes its END's lower bound, which for final bounds \
holds only under the RMAX1 assumption; whiskers out to the outer bounds, open arrows where a side has no bound; the \
details keep each zone's causal-only bounds; the label is the zone's count in SPU decrementer ticks; zones with an \
UNPLACED endpoint are counted in the lane's status line, never listed; hover a bar for its pair, click it for the \
details panel.";

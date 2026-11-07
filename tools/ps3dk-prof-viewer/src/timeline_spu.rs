//! Timeline rows for SPU lanes of a capture WITH SPU order records (file_flags bit 2; spu-ticks slice design 9.4 and
//! delta D1.6).  No egui types.
//!
//! The SPU ticks analysis ([`ps3dk_prof::spu_ticks`]) is built once per Document, on first need, in [`SpuSlot`] (a
//! refusal or an exhausted budget is cached as that result and never rebuilt by the viewer).  Each zone pair becomes
//! a bar from its endpoints' causal placements: begin in [L(BEGIN), U(BEGIN)], end in [L(END), U(END)]; the solid
//! core is drawn only where U(BEGIN) < L(END), the whiskers reach L(BEGIN) and U(END), and a side without a bound
//! runs to the chart edge as an open arrow.  A zone with an UNPLACED endpoint is never drawn and never listed: the
//! lane's one status line counts it ([`SpuTrackInfo::header`]).  The per-pair text lives in the bar's tooltip and
//! in the details panel a click opens ([`details`]).  The label is the zone's tick count in SPU decrementer ticks;
//! the bar's length is a placement envelope, never labelled as anything else.
//!
//! A snapshot of a capture that is still arriving is analysed with `Options::provisional` (set by the live
//! client): its lanes without ORDER_FINAL are placed from the order records received so far, and the status line
//! and tooltips say PROVISIONAL.
//!
//! Files without order records keep the clock-mapping rows of [`crate::timeline_foreign`] unchanged.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use ps3dk_prof::clockmap::{End, Projection, Rat};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState, TimelineBudgets};
use ps3dk_prof::spu_ticks::{
    analyze, conversion_sentence, convert, LaneOut, LaneState, Options, PairOut, Place, SpuResult, TickState,
};
use ps3dk_prof::Analysis;

/// The analysis options a cached result was built with (delta D1.6): the capture budget, the table identity and
/// the premise selection.  The isolation-scale option is formatting only and is not part of the key.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpuKey {
    pub capture_ops: u64,
    pub provisional: bool,
    /// The reviewed SPU ticks entries, by (profile id, accepted digests), in table order.
    pub table: Vec<(String, Vec<[u8; 32]>)>,
    pub spu_nowrap: bool,
}

impl SpuKey {
    pub fn of(o: &Options) -> SpuKey {
        SpuKey {
            capture_ops: o.capture_ops,
            provisional: o.provisional,
            table: o.table.iter().map(|e| (e.entry.profile_id.clone(), e.entry.digests.clone())).collect(),
            spu_nowrap: o.spu_nowrap,
        }
    }
}

/// The cached analysis of one Document.
#[derive(Debug, Clone)]
pub struct SpuView {
    /// None: the file has no domain extensions.
    pub result: Option<SpuResult>,
    /// (lane, BEGIN seq) -> the pair's depth, from the decoded pairs (built once with the result).
    pub depth: BTreeMap<(u32, u64), u8>,
    /// lane -> its image index.
    pub image: BTreeMap<u32, u32>,
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
        let mut depth = BTreeMap::new();
        let mut image = BTreeMap::new();
        if let Some(x) = &a.capture.ext {
            for (&l, li) in &x.lanes {
                image.insert(l, li.image_index);
            }
            for s in &x.sources {
                if let SourceKey::Lane(l) = s.key {
                    for p in &s.raw.pairs {
                        depth.insert((l, p.begin), p.depth);
                    }
                }
            }
        }
        let v = Arc::new(SpuView { result: analyze(a, &o), depth, image });
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
/// info of one lane.  `retain` is the number of bars this lane may still keep under the capture's cap.
pub fn lane_source(view: &Arc<SpuView>, lane: usize, retain: u64) -> (FtSource, SpuTrackInfo) {
    let r = view.result.as_ref().expect("a result");
    let u = &r.lanes[lane];
    let mut drawable: Vec<Drawable> = Vec::new();
    let (mut unplaced, mut placed) = (0u64, 0u64);
    let mut widths: Vec<u64> = Vec::new();
    for (i, p) in u.pairs_out.iter().enumerate() {
        for pl in [&p.place_b, &p.place_e] {
            if let Place::Placed(l, h) = pl {
                widths.push(h - l);
            }
        }
        if matches!((&p.place_b, &p.place_e), (Place::Placed(..), Place::Placed(..))) {
            placed += 1;
        }
        match (lu(&p.place_b), lu(&p.place_e)) {
            (Some((bl, bu)), Some((el, eu))) => drawable.push((i, bl, bu, el, eu)),
            _ => unplaced += 1,
        }
    }
    widths.sort_unstable();
    let median_width = (!widths.is_empty()).then(|| widths[(widths.len() - 1) / 2]);
    drawable.sort_by(|x, y| {
        let kx = (x.1.is_some(), x.1.unwrap_or(0), x.2.is_none(), x.2.unwrap_or(0), u.pairs_out[x.0].epoch, x.0);
        let ky = (y.1.is_some(), y.1.unwrap_or(0), y.2.is_none(), y.2.unwrap_or(0), u.pairs_out[y.0].epoch, y.0);
        kx.cmp(&ky)
    });
    let keep = (drawable.len() as u64).min(retain) as usize;
    let not_retained = (drawable.len() - keep) as u64;
    drawable.truncate(keep);
    let image = view.image.get(&u.lane_id).copied();
    let bars: Vec<FtBar> = drawable
        .iter()
        .map(|&(i, bl, bu, el, eu)| {
            let p = &u.pairs_out[i];
            FtBar {
                epoch: p.epoch,
                image,
                desc: p.desc,
                depth: view.depth.get(&(u.lane_id, p.begin)).copied().unwrap_or(0),
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
    let counts = FtCounts {
        placed: match &st {
            FtSourceState::Bars(v) => v.len() as u64,
            FtSourceState::Refused(_) => 0,
        },
        extrapolated: 0,
        too_deep: 0,
        not_placed: 0,
        open: None,
        censored: 0,
        unpaired: 0,
        loss_records: None,
        inventory_epochs: 0,
        no_inventory_epochs: 0,
    };
    let src = FtSource { source: SourceKey::Lane(u.lane_id), epochs: Vec::new(), counts, state: st };
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
    };
    (src, info)
}

/// The synthetic sources of every SPU lane of the result, and their track infos (keyed by lane id), under the
/// capture's retained-bar cap.
pub fn sources(view: &Arc<SpuView>, budgets: &TimelineBudgets) -> (Vec<FtSource>, BTreeMap<u32, SpuTrackInfo>) {
    let mut out = Vec::new();
    let mut infos = BTreeMap::new();
    let Some(r) = &view.result else { return (out, infos) };
    let mut left = budgets.retained_bars;
    for (i, u) in r.lanes.iter().enumerate() {
        let (s, info) = lane_source(view, i, left);
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

/// Placement text (design 9.3): "placed between L and U (causal bounds)".
pub fn place_text(p: &Place) -> String {
    match p {
        Place::Placed(l, u) => format!("placed between {l} and {u} (causal bounds)"),
        Place::LowerOnly(l) => format!("LOWER-ONLY: placed after {l}, no upper bound (causal bounds)"),
        Place::UpperOnly(u) => format!("UPPER-ONLY: placed before {u}, no lower bound (causal bounds)"),
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
    let mut v = vec![
        zone.to_string(),
        format!("SPU lane {}, epoch {}, seq {}/{}", info.lane_out().lane_id, p.epoch, p.begin, p.end),
        format!("{} (SPU decrementer ticks)", tick_label(p)),
        format!("BEGIN {}", place_text(&p.place_b)),
        format!("END {}", place_text(&p.place_e)),
        format!(
            "placement width BEGIN {}, END {} (uncertainty of the placement, not a cost)",
            width(&p.place_b),
            width(&p.place_e)
        ),
    ];
    if info.provisional {
        v.push(PROVISIONAL_NOTE.to_string());
    }
    v
}

/// The tooltip and details line of a PROVISIONAL placement.
pub const PROVISIONAL_NOTE: &str = concat!(
    "PROVISIONAL: placed from the order records received so far (no ORDER_FINAL yet); the final placement is ",
    "decided when the capture ends and lies inside these bounds unless a later record contradicts them"
);

/// The caption added under the SPU group for a file with order records.
pub const CAPTION: &str = "SPU lanes with order records: each zone is placed by causal bounds only (no clock \
mapping, no rate); solid where its BEGIN surely precedes its END's lower bound, whiskers out to the outer bounds, \
open arrows where a side has no bound; the label is the zone's count in SPU decrementer ticks; zones with an \
UNPLACED endpoint are counted in the lane's status line, never listed; hover a bar for its pair, \
click it for the details panel.";

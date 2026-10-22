//! Section 9 of the format: validity, token resolution, frame attribution, censoring, frame elapsed, per-lane
//! exclusive attributed work W, zone statistics and A/B.
//!
//! W(O, k, i) is START-FRAME attributed: a span counts in the frame it started in, for its whole exclusive duration.
//! It is not clipped to the frame interval, so it can exceed the frame's elapsed time, and it is never summed across
//! incarnations.
//!
//! No value here is ever saturated: totals are accumulated exactly (u128), and a duration that cannot be computed
//! (a CLOCK_ANOMALY, END before BEGIN) is UNKNOWN, never 0.
//!
//! [`load`] is the whole-file verdict: the structural pass [`reader::read`] followed by [`analyze`], which applies token
//! rule 4.  A successful `read` alone is not a validity verdict.

use std::collections::BTreeMap;
use std::fmt;

use crate::model::*;
use crate::reader;

/// The outcome of one token reference (section 9, ordered rules 1-3; rule 4 is MALFORMED).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenOutcome {
    Resolved,
    UnresolvedIncomplete,
    UnresolvedLoss,
}

impl fmt::Display for TokenOutcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            TokenOutcome::Resolved => "RESOLVED",
            TokenOutcome::UnresolvedIncomplete => "UNRESOLVED-INCOMPLETE",
            TokenOutcome::UnresolvedLoss => "UNRESOLVED-LOSS",
        })
    }
}

/// The frame a span is attributed to (start-frame attribution).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Attribution {
    Frame { owner: u32, frame: u64 },
    /// The (owner, frame) is known, but the owner is not MEASUREMENT VALID.
    OwnerInvalid { owner: u32, frame: u64 },
    /// A token whose FRAME is not in the file.
    Unresolved { owner: u32, frame: u64, outcome: TokenOutcome },
    /// UNKNOWN(pre-frame): an un-tokened owner span before the owner's first FRAME.
    PreFrame,
    /// UNKNOWN(loss): an un-tokened owner span after a GAP and before the next FRAME.
    Loss,
    /// UNKNOWN: an un-tokened span on an incarnation that emits no FRAME records.
    NotOwner,
}

impl Attribution {
    /// The (owner, frame) the span claims, if any.
    pub fn claim(&self) -> Option<(u32, u64)> {
        match *self {
            Attribution::Frame { owner, frame }
            | Attribution::OwnerInvalid { owner, frame }
            | Attribution::Unresolved { owner, frame, .. } => Some((owner, frame)),
            _ => None,
        }
    }
}

impl fmt::Display for Attribution {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Attribution::Frame { owner, frame } => write!(f, "frame {frame} of {owner}"),
            Attribution::OwnerInvalid { owner, frame } => write!(f, "frame {frame} of {owner}: owner invalid"),
            Attribution::Unresolved { owner, frame, outcome } => {
                write!(f, "unknown ({outcome}: frame {frame} of {owner})")
            }
            Attribution::PreFrame => write!(f, "UNKNOWN(pre-frame)"),
            Attribution::Loss => write!(f, "UNKNOWN(loss)"),
            Attribution::NotOwner => write!(f, "UNKNOWN(not a frame owner)"),
        }
    }
}

/// "spans N frames" (section 9): known only for un-tokened owner spans with no loss between BEGIN and END.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SpansN {
    Known(u64),
    Unknown,
}

impl fmt::Display for SpansN {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SpansN::Known(n) => write!(f, "spans {n} frames"),
            SpansN::Unknown => write!(f, "spans N: unknown"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneView {
    pub incarnation: u32,
    /// MEASUREMENT VALID.
    pub valid: bool,
    /// Why not valid; empty when valid.
    pub reasons: Vec<String>,
    /// The lane's own stream can be missing records (GAP, dropped > 0, or NOT_QUIESCED).
    pub loss: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpanView {
    pub attribution: Attribution,
    pub spans_n: SpansN,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TokenRef {
    pub span: usize,
    pub owner: u32,
    pub frame: u64,
    pub outcome: TokenOutcome,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FrameElapsed {
    pub owner: u32,
    pub frame: u64,
    pub value: Metric<u64>,
}

/// W(owner, frame, lane): one lane's start-frame-attributed exclusive work for one owner frame (exact, in ticks).
#[derive(Debug, Clone, PartialEq)]
pub struct Work {
    pub owner: u32,
    pub frame: u64,
    pub lane: u32,
    pub value: Metric<u128>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ZoneStats {
    pub name: String,
    pub logical_tag: String,
    /// Descriptors sharing this logical key; more than one refuses A/B.
    pub descriptors: usize,
    pub instances: u64,
    pub completed: u64,
    pub censored: u64,
    /// Ended instances whose duration is unavailable (END tb before BEGIN tb).  Excluded from every value.
    pub unavailable: u64,
    /// Exact sum of the completed instances' inclusive durations, in ticks.
    pub total: Metric<u128>,
    pub min: Metric<u64>,
    pub max: Metric<u64>,
    pub mean: Metric<f64>,
    pub p50: Metric<u64>,
    pub p90: Metric<u64>,
    pub p99: Metric<u64>,
    /// "completed instances only: k censored of n" when any instance is censored.
    pub label: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Analysis {
    pub capture: Capture,
    pub verdict: Verdict,
    pub lanes: BTreeMap<u32, LaneView>,
    /// Parallel to `capture.spans`.
    pub spans: Vec<SpanView>,
    pub tokens: Vec<TokenRef>,
    pub frames: Vec<FrameElapsed>,
    pub work: Vec<Work>,
    pub zones: Vec<ZoneStats>,
}

impl Analysis {
    pub fn lane_valid(&self, inc: u32) -> bool {
        self.lanes.get(&inc).is_some_and(|l| l.valid)
    }
    pub fn zone(&self, name: &str) -> Option<&ZoneStats> {
        self.zones.iter().find(|z| z.name == name)
    }
    pub fn work_for(&self, owner: u32, frame: u64, lane: u32) -> Option<&Metric<u128>> {
        self.work.iter().find(|w| w.owner == owner && w.frame == frame && w.lane == lane).map(|w| &w.value)
    }
    pub fn elapsed_for(&self, owner: u32, frame: u64) -> Option<&Metric<u64>> {
        self.frames.iter().find(|e| e.owner == owner && e.frame == frame).map(|e| &e.value)
    }
}

/// The result of reading a file: MALFORMED, or an analysis of a COMPLETE or INCOMPLETE capture.
#[derive(Debug, Clone)]
pub enum Outcome {
    Malformed(Malformed),
    Analysed(Box<Analysis>),
}

pub fn load(bytes: &[u8]) -> Outcome {
    match reader::read(bytes).and_then(analyze) {
        Ok(a) => Outcome::Analysed(Box::new(a)),
        Err(m) => Outcome::Malformed(m),
    }
}

fn lane_view(inc: u32, lane: &Lane, complete: bool) -> LaneView {
    let mut reasons = Vec::new();
    if !complete {
        reasons.push("capture not TRANSPORT COMPLETE".to_string());
    }
    let mut loss = lane.gap_count > 0;
    match &lane.final_entry {
        None => reasons.push("no final entry".to_string()),
        Some(e) if e.state == STATE_NOT_QUIESCED => {
            loss = true;
            reasons.push("NOT_QUIESCED".to_string());
        }
        Some(e) => {
            if e.dropped_count > 0 {
                loss = true;
                reasons.push(format!("dropped {}", e.dropped_count));
            }
            if e.refused_count > 0 {
                reasons.push(format!("refused {}", e.refused_count));
            }
            if e.regerr_count > 0 {
                reasons.push(format!("regerr {}", e.regerr_count));
            }
            if e.limit_flags & LIMIT_SEQ_LIMIT != 0 {
                reasons.push("SEQ_LIMIT".to_string());
            }
        }
    }
    if lane.gap_count > 0 {
        reasons.push(format!("{} GAP(s)", lane.gap_count));
    }
    if lane.clock_anomaly {
        reasons.push("CLOCK_ANOMALY".to_string());
    }
    LaneView { incarnation: inc, valid: reasons.is_empty(), reasons, loss }
}

/// Token resolution, rules 1-4 in order (section 9).
fn resolve(cap: &Capture, owner: u32, frame: u64) -> Result<TokenOutcome, Malformed> {
    let o = &cap.lanes[&owner];
    if o.frames.iter().any(|f| f.id == frame) {
        return Ok(TokenOutcome::Resolved);
    }
    if !cap.has_trailer() || cap.torn() || o.final_entry.is_none() {
        return Ok(TokenOutcome::UnresolvedIncomplete);
    }
    let e = o.final_entry.as_ref().expect("checked");
    if o.gap_count > 0 || e.state == STATE_NOT_QUIESCED || e.dropped_count > 0 {
        return Ok(TokenOutcome::UnresolvedLoss);
    }
    malformed(
        Rule::TokenImpossible,
        format!("token names frame {frame} of {owner}, which a lossless, quiesced owner never wrote"),
    )
}

pub fn analyze(cap: Capture) -> Result<Analysis, Malformed> {
    let complete = cap.incomplete.is_empty();
    let verdict = if complete { Verdict::Complete } else { Verdict::Incomplete(cap.incomplete.clone()) };
    let lanes: BTreeMap<u32, LaneView> =
        cap.lanes.iter().map(|(&inc, lane)| (inc, lane_view(inc, lane, complete))).collect();
    let valid = |inc: u32| lanes.get(&inc).is_some_and(|l| l.valid);

    // Tokens and attribution.
    let mut tokens = Vec::new();
    let mut spans = Vec::with_capacity(cap.spans.len());
    for (idx, s) in cap.spans.iter().enumerate() {
        let lane = &cap.lanes[&s.incarnation];
        let attribution = if let Some((owner, frame)) = s.token {
            let outcome = resolve(&cap, owner, frame)?;
            tokens.push(TokenRef { span: idx, owner, frame, outcome });
            match outcome {
                TokenOutcome::Resolved if valid(owner) => Attribution::Frame { owner, frame },
                TokenOutcome::Resolved => Attribution::OwnerInvalid { owner, frame },
                other => Attribution::Unresolved { owner, frame, outcome: other },
            }
        } else if !lane.is_frame_owner() {
            Attribution::NotOwner
        } else {
            match s.frame_context {
                FrameContext::Frame(frame) if valid(s.incarnation) => {
                    Attribution::Frame { owner: s.incarnation, frame }
                }
                FrameContext::Frame(frame) => Attribution::OwnerInvalid { owner: s.incarnation, frame },
                FrameContext::PreFrame => Attribution::PreFrame,
                FrameContext::Loss => Attribution::Loss,
            }
        };
        let spans_n = if s.token.is_some() || !lane.is_frame_owner() || s.affected {
            SpansN::Unknown
        } else {
            match (s.frames_before_end, s.gaps_before_end) {
                (Some(fe), Some(ge)) if ge == s.gaps_before_begin => fe
                    .checked_sub(s.frames_before_begin)
                    .and_then(|d| d.checked_add(1))
                    .map_or(SpansN::Unknown, SpansN::Known),
                _ => SpansN::Unknown,
            }
        };
        spans.push(SpanView { attribution, spans_n });
    }

    // Frame elapsed.
    let mut frames = Vec::new();
    for (&owner, lane) in &cap.lanes {
        for (i, fr) in lane.frames.iter().enumerate() {
            let next = lane.frames.get(i + 1).filter(|n| fr.id.checked_add(1) == Some(n.id));
            let value = if !valid(owner) {
                Metric::Refused("owner invalid".to_string())
            } else if let Some(n) = next {
                match n.tb.checked_sub(fr.tb) {
                    Some(d) => Metric::Value(d),
                    None => Metric::Unknown("CLOCK_ANOMALY: the next FRAME is earlier".to_string()),
                }
            } else {
                Metric::Unknown("censored: unclosed frame".to_string())
            };
            frames.push(FrameElapsed { owner, frame: fr.id, value });
        }
    }

    // W(O, k, i): exclusive durations, per lane, by start frame.
    #[derive(Default)]
    struct Acc {
        // u128: the exact sum of u64 durations (a capture holds fewer than 2^63 spans, so it cannot overflow).
        sum: u128,
        censored: bool,
        unresolved: bool,
        unavailable: bool,
    }
    // Key order: (owner u32, frame u64, lane u32), the field order of `Work`.
    let mut acc: BTreeMap<(u32, u64, u32), Acc> = BTreeMap::new();
    for (s, view) in cap.spans.iter().zip(&spans) {
        let Some((owner, frame)) = view.attribution.claim() else { continue };
        let a = acc.entry((owner, frame, s.incarnation)).or_default();
        if matches!(view.attribution, Attribution::Unresolved { .. }) {
            a.unresolved = true;
        }
        if s.is_censored() {
            a.censored = true;
        } else {
            match s.exclusive() {
                Some(x) => a.sum += u128::from(x),
                None => a.unavailable = true,
            }
        }
    }
    let work: Vec<Work> = acc
        .into_iter()
        .map(|((owner, frame, lane), a)| {
            let mut why = Vec::new();
            if !valid(owner) {
                why.push("owner invalid");
            }
            if !valid(lane) {
                why.push("lane invalid");
            }
            if a.censored {
                why.push("censored");
            }
            if a.unresolved {
                why.push("unresolved");
            }
            if a.unavailable {
                why.push("duration unavailable");
            }
            let value = if why.is_empty() { Metric::Value(a.sum) } else { Metric::Refused(why.join(", ")) };
            Work { owner, frame, lane, value }
        })
        .collect();

    let zones = zone_stats(&cap, &valid);
    Ok(Analysis { capture: cap, verdict, lanes, spans, tokens, frames, work, zones })
}

fn zone_stats(cap: &Capture, valid: &dyn Fn(u32) -> bool) -> Vec<ZoneStats> {
    #[derive(Default)]
    struct Z {
        instances: u64,
        censored: u64,
        unavailable: u64,
        durations: Vec<u64>,
    }
    let mut keys: BTreeMap<(String, String), usize> = BTreeMap::new();
    for d in cap.descriptors.values() {
        *keys.entry((d.name.clone(), d.logical_tag.clone())).or_default() += 1;
    }
    let mut zones: BTreeMap<(String, String), Z> = BTreeMap::new();
    for s in &cap.spans {
        if !valid(s.incarnation) {
            continue;
        }
        let d = &cap.descriptors[&s.desc];
        let z = zones.entry((d.name.clone(), d.logical_tag.clone())).or_default();
        z.instances += 1;
        if s.is_censored() {
            z.censored += 1;
        } else if let Some(x) = s.inclusive() {
            z.durations.push(x);
        } else {
            z.unavailable += 1;
        }
    }
    zones
        .into_iter()
        .map(|((name, logical_tag), mut z)| {
            z.durations.sort_unstable();
            let k = z.durations.len() as u64;
            let none = || Metric::Unknown("no completed instances".to_string());
            let (total, min, max, mean) = if k == 0 {
                (
                    Metric::Unknown("no completed instances".to_string()),
                    none(),
                    none(),
                    Metric::Unknown("no completed instances".to_string()),
                )
            } else {
                // Exact: the sum of fewer than 2^63 u64 values fits in u128.
                let total: u128 = z.durations.iter().map(|&x| u128::from(x)).sum();
                (
                    Metric::Value(total),
                    Metric::Value(z.durations[0]),
                    Metric::Value(z.durations[z.durations.len() - 1]),
                    Metric::Value(total as f64 / k as f64),
                )
            };
            let pct = |p: u64| {
                if k < 20 {
                    Metric::Refused(format!("{k} completed instances, percentiles need 20"))
                } else {
                    let rank = (p * k).div_ceil(100).max(1);
                    Metric::Value(z.durations[(rank - 1) as usize])
                }
            };
            let label = match (z.censored, z.unavailable) {
                (0, 0) => None,
                (c, 0) => Some(format!("completed instances only: {c} censored of {}", z.instances)),
                (c, u) => Some(format!(
                    "completed instances only: {c} censored and {u} with an unavailable duration of {}",
                    z.instances
                )),
            };
            ZoneStats {
                descriptors: keys.get(&(name.clone(), logical_tag.clone())).copied().unwrap_or(0),
                name,
                logical_tag,
                instances: z.instances,
                completed: k,
                censored: z.censored,
                unavailable: z.unavailable,
                total,
                min,
                max,
                mean,
                p50: pct(50),
                p90: pct(90),
                p99: pct(99),
                label,
            }
        })
        .collect()
}

#[derive(Debug, Clone, PartialEq)]
pub enum AbResult {
    /// Mean inclusive duration of the completed instances in each capture, in ticks of [`Comparison::freq_hz`].  Only
    /// in a [`ComparisonKind::Compatible`] comparison.
    Compared { a_mean: f64, b_mean: f64 },
    /// The same means, in a [`ComparisonKind::DescriptiveCrossWorkload`] comparison: the captures ran different
    /// workloads or apps, so this is a description of two runs, NOT an A/B result.
    Descriptive { a_mean: f64, b_mean: f64 },
    Refused(String),
    /// The key has spans in a valid lane of A and no span at all in B.
    OnlyInA,
    /// The key has spans in a valid lane of B and no span at all in A.
    OnlyInB,
    /// The key has spans, but only in INVALID lanes of the capture(s) flagged: present, yet not comparable.  Not the
    /// same as absent.
    InvalidLanesOnly { a: bool, b: bool },
}

#[derive(Debug, Clone, PartialEq)]
pub struct AbRow {
    pub name: String,
    pub logical_tag: String,
    pub result: AbResult,
    /// Recorded, intentional differences for this key (source file and line); never a refusal.
    pub differences: Vec<String>,
}

/// Caller policy for [`compare_with`].  The default refuses captures of different workloads or apps.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CompareOptions {
    /// Compare captures whose workload tag or app differs.  The result is then a
    /// [`ComparisonKind::DescriptiveCrossWorkload`], never an ordinary A/B result.
    pub allow_cross_workload: bool,
}

/// What kind of comparison a [`Comparison`] is.  Reports print it FIRST.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ComparisonKind {
    /// An ordinary A/B: same workload and app, same ABI and clock, both COMPLETE.
    Compatible,
    /// The caller allowed different workloads or apps: a description of two runs, not an A/B result.
    DescriptiveCrossWorkload,
    /// Refused as a whole; see [`Comparison::refused`].
    Refused,
}

/// The result of an A/B comparison, with its compatibility metadata.
#[derive(Debug, Clone, PartialEq)]
pub struct Comparison {
    pub kind: ComparisonKind,
    /// A capture-level refusal (not COMPLETE, ABI, clock, workload or app mismatch): when set, every row is refused
    /// with it.
    pub refused: Option<String>,
    /// The workload and app differences that made this a descriptive cross-workload comparison (empty otherwise).
    pub cross_workload: Vec<String>,
    /// The clock frequency both captures share; every compared value is in ticks of this clock.
    pub freq_hz: Option<u64>,
    /// Recorded, intentional differences between the captures: the variables under test (build id, SDK version,
    /// compiler identity, flags).
    pub differences: Vec<String>,
    /// The MEASUREMENT VALID incarnations each side's statistics come from.
    pub a_scope: Vec<u32>,
    pub b_scope: Vec<u32>,
    pub rows: Vec<AbRow>,
}

/// Where a logical key appears in one capture.
#[derive(Clone, Copy)]
enum Presence<'a> {
    Valid(&'a ZoneStats),
    InvalidOnly,
    Absent,
}

fn scope_name(scope: u8) -> &'static str {
    if scope == 1 {
        "cpu"
    } else {
        "gpu-complete"
    }
}

/// (scopes, "file:line" locations) of the descriptors with this logical key.
fn key_descriptors(cap: &Capture, name: &str, tag: &str) -> (Vec<u8>, Vec<String>) {
    let mut scopes = Vec::new();
    let mut places = Vec::new();
    for d in cap.descriptors.values().filter(|d| d.name == name && d.logical_tag == tag) {
        if !scopes.contains(&d.scope) {
            scopes.push(d.scope);
        }
        places.push(format!("{}:{}", d.file, d.line));
    }
    scopes.sort_unstable();
    places.sort();
    (scopes, places)
}

/// [`compare_with`] under the default policy: different workloads or apps are REFUSED.
pub fn compare(a: &Analysis, b: &Analysis) -> Comparison {
    compare_with(a, b, CompareOptions::default())
}

/// A/B by logical key, over the MEASUREMENT VALID incarnations of each capture (section 9).  The compatibility
/// policy (tools/ps3dk-prof/README.md):
/// - REFUSED for the whole comparison: either capture not TRANSPORT COMPLETE; a different ABI; a different (or
///   missing) clock frequency (values are raw ticks, so captures are compared only on the same timebase); a
///   different workload tag or app, unless `opts.allow_cross_workload`.
/// - With `allow_cross_workload` and a workload or app difference, the comparison is
///   [`ComparisonKind::DescriptiveCrossWorkload`] and its rows are [`AbResult::Descriptive`], never `Compared`.
/// - REFUSED per key: a duplicate logical key; a different descriptor scope (cpu vs gpu-complete); censored instances;
///   no completed instances.
/// - RECORDED, not refused, as the intended variables: build id, SDK version, compiler identity and flags (per
///   capture), and the source file and line (per key).
pub fn compare_with(a: &Analysis, b: &Analysis, opts: CompareOptions) -> Comparison {
    let scope_of = |x: &Analysis| -> Vec<u32> { x.lanes.values().filter(|l| l.valid).map(|l| l.incarnation).collect() };
    let (ha, hb) = (a.capture.header.as_ref(), b.capture.header.as_ref());
    let (fa, fb) = (a.capture.clock.as_ref().map(|c| c.freq_hz), b.capture.clock.as_ref().map(|c| c.freq_hz));

    let mut cross_workload = Vec::new();
    if let (Some(x), Some(y)) = (ha, hb) {
        if x.workload_tag != y.workload_tag {
            cross_workload.push(format!("workload: A {:?}, B {:?}", x.workload_tag, y.workload_tag));
        }
        if x.app != y.app {
            cross_workload.push(format!("app: A {:?}, B {:?}", x.app, y.app));
        }
    }

    let refused = if a.verdict != Verdict::Complete {
        Some("capture A not TRANSPORT COMPLETE".to_string())
    } else if b.verdict != Verdict::Complete {
        Some("capture B not TRANSPORT COMPLETE".to_string())
    } else if ha.map(|h| h.abi) != hb.map(|h| h.abi) {
        Some("ABI differs".to_string())
    } else if fa.is_none() || fa != fb {
        Some(format!(
            "clock frequency differs (A {} Hz, B {} Hz); values are raw ticks",
            fa.map_or("unknown".to_string(), |f| f.to_string()),
            fb.map_or("unknown".to_string(), |f| f.to_string())
        ))
    } else if !cross_workload.is_empty() && !opts.allow_cross_workload {
        Some(format!(
            "different workload or app ({}); pass allow_cross_workload for a descriptive comparison",
            cross_workload.join("; ")
        ))
    } else {
        None
    };
    let kind = if refused.is_some() {
        ComparisonKind::Refused
    } else if !cross_workload.is_empty() {
        ComparisonKind::DescriptiveCrossWorkload
    } else {
        ComparisonKind::Compatible
    };

    let mut differences = Vec::new();
    if let (Some(x), Some(y)) = (ha, hb) {
        let mut note = |what: &str, p: String, q: String| {
            if p != q {
                differences.push(format!("{what}: A {p:?}, B {q:?}"));
            }
        };
        note("build id", hex(&x.build_id), hex(&y.build_id));
        note("sdk version", x.sdk_version.clone(), y.sdk_version.clone());
        note("compiler", x.compiler_identity.clone(), y.compiler_identity.clone());
        note("flags", x.flags.clone(), y.flags.clone());
    }

    // Every logical key with a span anywhere (valid or not) in either capture.
    let span_keys = |x: &Analysis| -> std::collections::BTreeSet<(String, String)> {
        x.capture
            .spans
            .iter()
            .filter_map(|s| x.capture.descriptors.get(&s.desc))
            .map(|d| (d.name.clone(), d.logical_tag.clone()))
            .collect()
    };
    let (ka, kb) = (span_keys(a), span_keys(b));

    let rows = ka
        .union(&kb)
        .map(|k| {
            let (name, logical_tag) = k.clone();
            let (sa, la) = key_descriptors(&a.capture, &name, &logical_tag);
            let (sb, lb) = key_descriptors(&b.capture, &name, &logical_tag);
            let mut row_diff = Vec::new();
            if !la.is_empty() && !lb.is_empty() && la != lb {
                row_diff.push(format!("source location: A {}, B {}", la.join(" "), lb.join(" ")));
            }
            let result = if let Some(why) = &refused {
                AbResult::Refused(why.clone())
            } else {
                match (presence(a, &ka, k), presence(b, &kb, k)) {
                    (Presence::Valid(x), Presence::Valid(y)) => {
                        if x.descriptors > 1 || y.descriptors > 1 {
                            AbResult::Refused("duplicate logical key".to_string())
                        } else if sa != sb {
                            let names = |s: &[u8]| s.iter().map(|&v| scope_name(v)).collect::<Vec<_>>().join("/");
                            AbResult::Refused(format!("descriptor scope differs (A {}, B {})", names(&sa), names(&sb)))
                        } else if x.censored > 0 || y.censored > 0 {
                            AbResult::Refused(format!(
                                "CENSORED (A: {} censored of {}, B: {} censored of {})",
                                x.censored, x.instances, y.censored, y.instances
                            ))
                        } else {
                            match (x.mean.value(), y.mean.value()) {
                                (Some(&am), Some(&bm)) if kind == ComparisonKind::Compatible => {
                                    AbResult::Compared { a_mean: am, b_mean: bm }
                                }
                                (Some(&am), Some(&bm)) => AbResult::Descriptive { a_mean: am, b_mean: bm },
                                _ => AbResult::Refused("no completed instances".to_string()),
                            }
                        }
                    }
                    (Presence::Valid(_), Presence::Absent) => AbResult::OnlyInA,
                    (Presence::Absent, Presence::Valid(_)) => AbResult::OnlyInB,
                    (pa, pb) => AbResult::InvalidLanesOnly {
                        a: matches!(pa, Presence::InvalidOnly),
                        b: matches!(pb, Presence::InvalidOnly),
                    },
                }
            };
            AbRow { name, logical_tag, result, differences: row_diff }
        })
        .collect();

    Comparison {
        kind,
        refused,
        cross_workload,
        freq_hz: fa.filter(|_| fa == fb),
        differences,
        a_scope: scope_of(a),
        b_scope: scope_of(b),
        rows,
    }
}

/// Valid statistics for key `k` in `x`; else present only in invalid lanes (a span with that key exists); else absent.
fn presence<'a>(
    x: &'a Analysis,
    keys: &std::collections::BTreeSet<(String, String)>,
    k: &(String, String),
) -> Presence<'a> {
    match x.zones.iter().find(|z| z.name == k.0 && z.logical_tag == k.1) {
        Some(z) => Presence::Valid(z),
        None if keys.contains(k) => Presence::InvalidOnly,
        None => Presence::Absent,
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

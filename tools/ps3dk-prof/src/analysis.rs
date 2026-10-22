//! Section 9 of the format: validity, token resolution, frame attribution, censoring, frame elapsed, per-lane
//! exclusive attributed work W, zone statistics and A/B.
//!
//! W(O, k, i) is START-FRAME attributed: a span counts in the frame it started in, for its whole exclusive duration.
//! It is not clipped to the frame interval, so it can exceed the frame's elapsed time, and it is never summed across
//! incarnations.

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

/// W(owner, frame, lane): one lane's start-frame-attributed exclusive work for one owner frame.
#[derive(Debug, Clone, PartialEq)]
pub struct Work {
    pub owner: u32,
    pub frame: u64,
    pub lane: u32,
    pub value: Metric<u64>,
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
    pub total: Metric<u64>,
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
    pub fn work_for(&self, owner: u32, frame: u64, lane: u32) -> Option<&Metric<u64>> {
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
                (Some(fe), Some(ge)) if ge == s.gaps_before_begin => {
                    SpansN::Known(1 + fe.saturating_sub(s.frames_before_begin))
                }
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
                Metric::Value(n.tb.saturating_sub(fr.tb))
            } else {
                Metric::Unknown("censored: unclosed frame".to_string())
            };
            frames.push(FrameElapsed { owner, frame: fr.id, value });
        }
    }

    // W(O, k, i): exclusive durations, per lane, by start frame.
    #[derive(Default)]
    struct Acc {
        sum: u64,
        censored: bool,
        unresolved: bool,
    }
    let mut acc: BTreeMap<(u32, u64, u32), Acc> = BTreeMap::new();
    for (s, view) in cap.spans.iter().zip(&spans) {
        let Some((owner, frame)) = view.attribution.claim() else { continue };
        let a = acc.entry((owner, frame, s.incarnation)).or_default();
        if matches!(view.attribution, Attribution::Unresolved { .. }) {
            a.unresolved = true;
        }
        if s.is_censored() {
            a.censored = true;
        } else if let Some(x) = s.exclusive() {
            a.sum = a.sum.saturating_add(x);
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
        }
    }
    zones
        .into_iter()
        .map(|((name, logical_tag), mut z)| {
            z.durations.sort_unstable();
            let k = z.durations.len() as u64;
            let none = || Metric::Unknown("no completed instances".to_string());
            let (total, min, max, mean) = if k == 0 {
                (none(), none(), none(), Metric::Unknown("no completed instances".to_string()))
            } else {
                let total: u64 = z.durations.iter().fold(0u64, |acc, &x| acc.saturating_add(x));
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
            let label = (z.censored > 0)
                .then(|| format!("completed instances only: {} censored of {}", z.censored, z.instances));
            ZoneStats {
                descriptors: keys.get(&(name.clone(), logical_tag.clone())).copied().unwrap_or(0),
                name,
                logical_tag,
                instances: z.instances,
                completed: k,
                censored: z.censored,
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
    /// Mean inclusive duration of the completed instances in each capture.
    Compared { a_mean: f64, b_mean: f64 },
    Refused(String),
    OnlyInA,
    OnlyInB,
}

#[derive(Debug, Clone, PartialEq)]
pub struct AbRow {
    pub name: String,
    pub logical_tag: String,
    pub result: AbResult,
}

/// A/B by logical key, over the MEASUREMENT VALID incarnations of each capture (section 9).
pub fn compare(a: &Analysis, b: &Analysis) -> Vec<AbRow> {
    let key = |z: &ZoneStats| (z.name.clone(), z.logical_tag.clone());
    let mut rows: BTreeMap<(String, String), (Option<&ZoneStats>, Option<&ZoneStats>)> = BTreeMap::new();
    for z in &a.zones {
        rows.entry(key(z)).or_default().0 = Some(z);
    }
    for z in &b.zones {
        rows.entry(key(z)).or_default().1 = Some(z);
    }
    rows.into_iter()
        .map(|((name, logical_tag), (za, zb))| {
            let result = if a.verdict != Verdict::Complete {
                AbResult::Refused("capture A not TRANSPORT COMPLETE".to_string())
            } else if b.verdict != Verdict::Complete {
                AbResult::Refused("capture B not TRANSPORT COMPLETE".to_string())
            } else {
                match (za, zb) {
                    (Some(_), None) => AbResult::OnlyInA,
                    (None, Some(_)) => AbResult::OnlyInB,
                    (None, None) => unreachable!(),
                    (Some(x), Some(y)) => {
                        if x.descriptors > 1 || y.descriptors > 1 {
                            AbResult::Refused("duplicate logical key".to_string())
                        } else if x.censored > 0 || y.censored > 0 {
                            AbResult::Refused(format!(
                                "CENSORED (A: {} censored of {}, B: {} censored of {})",
                                x.censored, x.instances, y.censored, y.instances
                            ))
                        } else {
                            match (x.mean.value(), y.mean.value()) {
                                (Some(&am), Some(&bm)) => AbResult::Compared { a_mean: am, b_mean: bm },
                                _ => AbResult::Refused("no completed instances".to_string()),
                            }
                        }
                    }
                }
            };
            AbRow { name, logical_tag, result }
        })
        .collect()
}

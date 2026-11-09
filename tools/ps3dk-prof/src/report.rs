//! The text report of `ps3dk-prof report`.  Refused and unknown values are printed as words, never as numbers.

use std::fmt::Write;

use crate::analysis::{AbResult, Analysis, Comparison, ComparisonKind};
use crate::model::*;

/// The first line of every comparison report: what kind of comparison it is.
pub fn comparison_kind_line(c: &Comparison) -> String {
    match c.kind {
        ComparisonKind::Compatible => "A/B COMPARISON (compatible captures)".to_string(),
        ComparisonKind::DescriptiveCrossWorkload => format!(
            "DESCRIPTIVE CROSS-WORKLOAD COMPARISON, NOT AN A/B RESULT ({})",
            c.cross_workload.join("; ")
        ),
        ComparisonKind::Refused => {
            format!("COMPARISON REFUSED: {}", c.refused.as_deref().unwrap_or("no reason recorded"))
        }
    }
}

/// The text of a comparison.  Its kind is printed FIRST, before any value.
pub fn render_comparison(c: &Comparison) -> String {
    let mut o = String::new();
    let _ = writeln!(o, "{}", comparison_kind_line(c));
    if let Some(f) = c.freq_hz {
        let _ = writeln!(o, "clock: {f} Hz; means are in timebase ticks");
    }
    let list = |v: &[u32]| -> String {
        if v.is_empty() {
            "none".to_string()
        } else {
            v.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", ")
        }
    };
    let _ = writeln!(o, "scope: A lanes [{}], B lanes [{}]", list(c.a_scope.as_slice()), list(c.b_scope.as_slice()));
    for d in &c.differences {
        let _ = writeln!(o, "recorded difference: {d}");
    }
    for r in &c.rows {
        let tag = if r.logical_tag.is_empty() { String::new() } else { format!(" [{}]", r.logical_tag) };
        let what = match &r.result {
            AbResult::Compared { a_mean, b_mean } => format!("A mean {a_mean}, B mean {b_mean}"),
            AbResult::Descriptive { a_mean, b_mean } => {
                format!("descriptive only: A mean {a_mean}, B mean {b_mean}")
            }
            AbResult::Refused(why) => format!("refused ({why})"),
            AbResult::OnlyInA => "only in A".to_string(),
            AbResult::OnlyInB => "only in B".to_string(),
            AbResult::InvalidLanesOnly { a, b } => format!(
                "present only in invalid lanes of{}{}",
                if *a { " A" } else { "" },
                if *b { " B" } else { "" }
            ),
        };
        let _ = writeln!(o, "  {}{tag}: {what}", r.name);
        for d in &r.differences {
            let _ = writeln!(o, "    recorded difference: {d}");
        }
    }
    o
}

pub const W_NOTE: &str = "W = start-frame-attributed EXCLUSIVE work of one lane: each span counts in the frame it \
started in, for its whole exclusive duration. W is not clipped to the frame interval, can exceed the frame's \
elapsed time, and is never summed across lanes.";

/// The extension layer (file_flags bit 0): one line per extension source, after the whole-file and PPU layers.
/// Empty for a v1 file. A TRANSPORT COMPLETE file can still list sources that are INCOMPLETE or NOT VALID.
/// The words of one extension source's verdict layers, shared by the text report and the viewer (so they cannot
/// diverge).  Each text is exactly what the report prints for that layer; the booleans carry the semantics, so no
/// consumer has to infer them from the text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceParts {
    /// e.g. "SPU lane 10" or "GPU domain 3"
    pub source: String,
    pub is_lane: bool,
    pub domain: u32,
    pub structure_complete: bool,
    /// "COMPLETE" or "INCOMPLETE (reasons)"
    pub structure: String,
    pub raw_valid: bool,
    /// "VALID" or "NOT VALID (reasons)"
    pub raw: String,
    pub coverage_known: bool,
    /// "no raw gap or unlocatable event" or "UNKNOWN for the whole capture (reasons)"
    pub coverage: String,
    pub events: u64,
    pub lost: u64,
}

pub fn source_parts(s: &crate::ext::SourceReport) -> SourceParts {
    use crate::ext::{SourceKey, Structure};
    SourceParts {
        source: s.key.to_string(),
        is_lane: matches!(s.key, SourceKey::Lane(_)),
        domain: s.domain_id,
        structure_complete: s.structure == Structure::Complete,
        structure: match s.structure {
            Structure::Complete => "COMPLETE".to_string(),
            Structure::Incomplete => format!("INCOMPLETE ({})", s.incomplete.join("; ")),
        },
        raw_valid: s.raw_valid,
        raw: if s.raw_valid { "VALID".to_string() } else { format!("NOT VALID ({})", s.invalid.join("; ")) },
        coverage_known: s.coverage_unknown.is_empty(),
        coverage: if s.coverage_unknown.is_empty() {
            "no raw gap or unlocatable event".to_string()
        } else {
            format!("UNKNOWN for the whole capture ({})", s.coverage_unknown.join("; "))
        },
        events: s.events_total,
        lost: s.lost_total,
    }
}

/// The words of one declared epoch of an SPU lane (the counter_variation() result), shared by report and viewer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpochParts {
    pub epoch: u32,
    pub state: crate::ext::CounterVariation,
    /// The state with its complete observed qualifier, e.g. "COUNTER_STATIC (all observed values equal; says nothing
    /// about unobserved intervals)".
    pub state_text: &'static str,
    /// "N events + M brackets"
    pub observations: String,
    /// The count of LOSS records inside the observed event-sequence span, or "NOT AVAILABLE (fewer than 2 events)".
    pub loss_span: String,
    /// "source RAW VALIDITY is NOT VALID: reasons" when the source is not RAW VALID: an invalid source never gets an
    /// epoch view that looks valid.
    pub invalidity: Option<String>,
}

/// The per-epoch view of one extension source.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LaneEpochs {
    /// A GPU domain (or any non-lane source): source facts only.
    NotALane,
    /// counter_variation() refused (an event outside the declared epochs: not guessed).
    Inconsistent,
    Epochs(Vec<EpochParts>),
}

pub const EPOCH_LOSS_SPAN_LABEL: &str = "LOSS records inside the observed event-sequence span";
pub const EPOCH_TAIL: &str = "order and raw endpoints only (no durations)";

pub fn epoch_parts(x: &crate::ext::ExtCapture, s: &crate::ext::SourceReport) -> LaneEpochs {
    use crate::ext::{counter_variation, CounterVariation, SourceKey};
    if !matches!(s.key, SourceKey::Lane(_)) {
        return LaneEpochs::NotALane;
    }
    let Some(v) = counter_variation(x, s) else { return LaneEpochs::Inconsistent };
    let invalidity =
        if s.raw_valid { None } else { Some(format!("source RAW VALIDITY is NOT VALID: {}", s.invalid.join("; "))) };
    LaneEpochs::Epochs(
        v.iter()
            .map(|e| EpochParts {
                epoch: e.epoch,
                state: e.state,
                state_text: match e.state {
                    CounterVariation::Insufficient => "INSUFFICIENT (fewer than 2 observations)",
                    CounterVariation::Static => {
                        "COUNTER_STATIC (all observed values equal; says nothing about unobserved intervals)"
                    }
                    CounterVariation::Moving => {
                        "COUNTER_MOVING (variation observed; not proof of continuous counting)"
                    }
                },
                observations: format!("{} events + {} brackets", e.events, e.brackets),
                loss_span: match e.loss_records_in_event_span {
                    Some(k) => format!("{k}"),
                    None => "NOT AVAILABLE (fewer than 2 events)".to_string(),
                },
                invalidity: invalidity.clone(),
            })
            .collect(),
    )
}

pub const EXT_EMPTY_LINE: &str =
    "domain extensions: 0 sources declared (no SPU/RSX work is recorded; no SPU/RSX coverage is claimed)";

pub fn extension_lines(cap: &Capture) -> Vec<String> {
    let mut out = domain_lines(cap);
    if let Some(w) = &cap.waits {
        let status = if let Some(why) = w.refusal { why }
            else if w.config.is_none() || !w.status_present { "INCOMPLETE: config or status missing" }
            else { "status RECORDED; counters may be lower bounds" };
        out.push(format!("wait observations: {status}; {} retained call(s); dependency UNKNOWN; use waits for API-call elapsed time and coverage",w.observations));
    }
    out
}

fn domain_lines(cap: &Capture) -> Vec<String> {
    let Some(x) = &cap.ext else { return Vec::new() };
    if x.sources.is_empty() {
        return vec![EXT_EMPTY_LINE.to_string()];
    }
    let mut out = vec![format!(
        "domain extensions: {} source(s); MAPPING is UNMEASURED for every source (no SPU/RSX time is claimed)",
        x.sources.len()
    )];
    for s in &x.sources {
        out.extend(source_fact_lines(s));
        out.extend(counter_lines(x, s));
    }
    out
}

/// The two fact lines of one extension source (its verdict layers, then its counts), exactly as `check` and
/// `report` print them; `ps3dk-prof clockmap` prints them too, before the source's conditional results.
pub fn source_fact_lines(s: &crate::ext::SourceReport) -> Vec<String> {
    use crate::ext::OpenBegins;
    let mut out = Vec::with_capacity(2);
    let p = source_parts(s);
    let open = match s.open_begins {
        OpenBegins::Verified(n) => format!("open_begins {n} verified"),
        OpenBegins::Unverifiable(n) => format!("open_begins {n} UNVERIFIABLE"),
        OpenBegins::NoFinal => "no FINAL".to_string(),
    };
    out.push(format!(
        "  {} (domain {}): STRUCTURE {} | RAW VALIDITY {} | MAPPING UNMEASURED | COVERAGE {}",
        p.source, p.domain, p.structure, p.raw, p.coverage
    ));
    let at = if s.final_state.is_some() { "FINAL" } else { "end (no FINAL)" };
    let open_at = match s.open_at_end() {
        Some(n) => format!("open at {at} {n}"),
        None => format!("open at {at} UNKNOWN (protocol error)"),
    };
    let mut detail = format!(
        "    events {} lost {} | paired {} censored {} unpaired {} marks {} | {open_at} | {open} | epochs {} \
         brackets {}",
        s.events_total,
        s.lost_total,
        s.paired(),
        s.censored(),
        s.unpaired(),
        s.marks(),
        s.epochs,
        s.brackets
    );
    if matches!(s.key, crate::ext::SourceKey::Gpu(_)) {
        detail.push_str(&format!(
            " | frames resolved {} unresolved {} none {}",
            s.frames_resolved, s.frames_unresolved, s.frames_none
        ));
    }
    out.push(detail);
    out
}

/// One line per declared epoch of an SPU lane: the observed variation of its recorded decrementer values (no
/// durations; MAPPING stays UNMEASURED); a source that is not RAW VALID repeats why on every line.
fn counter_lines(x: &crate::ext::ExtCapture, s: &crate::ext::SourceReport) -> Vec<String> {
    match epoch_parts(x, s) {
        LaneEpochs::NotALane => Vec::new(),
        LaneEpochs::Inconsistent => {
            vec!["    counter variation: not classified (decoded epochs inconsistent)".to_string()]
        }
        LaneEpochs::Epochs(v) => v
            .iter()
            .map(|e| {
                let qual = e.invalidity.as_ref().map(|q| format!(" | {q}")).unwrap_or_default();
                format!(
                    "    epoch {}: {} | observations: {} | {EPOCH_LOSS_SPAN_LABEL}: {} | {EPOCH_TAIL}{qual}",
                    e.epoch, e.state_text, e.observations, e.loss_span
                )
            })
            .collect(),
    }
}

/// True when every extension source is STRUCTURE COMPLETE and RAW VALID (always true for a v1 file).
pub fn extensions_clean(cap: &Capture) -> bool {
    cap.ext
        .as_ref()
        .is_none_or(|x| x.sources.iter().all(|s| s.structure == crate::ext::Structure::Complete && s.raw_valid))
}

/// Like [`extensions_clean`], for an analysed file.  A capture that ended without a TRAILER cannot have DOMAIN_FINAL
/// records or a valid TRAILER, so its sources are clean when they pass the prefix-only raw validity rule (see
/// [`crate::spu_ticks::provisional_raw_ok`]): the missing ending is forgiven, a lost event, a protocol error or an
/// abandoned, overflowed or unbound source is not.
pub fn analysis_extensions_clean(a: &Analysis) -> bool {
    if a.prefix.is_none() {
        return extensions_clean(&a.capture);
    }
    a.capture.ext.as_ref().is_none_or(|x| x.sources.iter().all(crate::spu_ticks::provisional_raw_ok))
}

/// The note on a capture that ended without a TRAILER (None for any file that has one).
pub fn ending_note(a: &Analysis) -> Option<String> {
    let p = a.prefix.as_ref()?;
    let torn = if p.torn_dropped > 0 { format!(", {} bytes of a torn last chunk dropped", p.torn_dropped) } else { String::new() };
    Some(format!(
        "ended without a trailer (app quit or the link ended); analysed up to byte {} ({} whole chunks{torn}); loss not yet reported at the cut cannot be seen",
        p.cut, p.chunks
    ))
}

/// The verdict line of an analysed file: `verdict_line`, or COMPLETE with the ending note.
pub fn verdict_text(a: &Analysis) -> String {
    match ending_note(a) {
        Some(n) => format!("COMPLETE ({n})"),
        None => verdict_line(&a.verdict),
    }
}

/// The machine-readable ending, printed only for a file without a TRAILER (the verdict word stays COMPLETE).
pub const ENDING_NO_TRAILER: &str = "ending: NO-TRAILER";

/// The verdict line, plus the `ending:` line for a file without a TRAILER.
pub fn verdict_block(a: &Analysis) -> String {
    match a.prefix {
        Some(_) => format!("{}
{ENDING_NO_TRAILER}", verdict_text(a)),
        None => verdict_text(a),
    }
}

pub fn verdict_line(v: &Verdict) -> String {
    match v {
        Verdict::Complete => "COMPLETE (TRANSPORT COMPLETE)".to_string(),
        Verdict::Incomplete(why) => {
            let why: Vec<String> = why.iter().map(|w| w.to_string()).collect();
            format!("INCOMPLETE: {}", why.join("; "))
        }
    }
}

pub fn render(a: &Analysis) -> String {
    let mut o = String::new();
    let cap = &a.capture;
    let _ = writeln!(o, "verdict: {}", verdict_text(a));
    if a.prefix.is_some() {
        let _ = writeln!(o, "{ENDING_NO_TRAILER}");
    }
    if cap.minor_warning {
        let _ = writeln!(o, "warning: minor version > 0; read as v1");
    }
    if let Some(status) = cap.dependency_status() {
        let _ = writeln!(o, "{}", crate::dependencies::status_text(&status));
    }
    if let Some(evidence) = &cap.source_checkpoints {
        o.push_str(&crate::source_checkpoints::report(evidence));
    }
    // Only files with file_flags bit 1 carry a PROFILE; every other file's report is unchanged.
    if let Some(p) = &cap.profile {
        let _ = writeln!(o, "{}", crate::profile::record_line(p));
    }
    if let Some(h) = &cap.header {
        let _ = writeln!(
            o,
            "app {} | abi {} | sdk {} | compiler {} | workload {}",
            h.app,
            if h.abi == 1 { "ILP32" } else { "LP64" },
            h.sdk_version,
            h.compiler_identity,
            h.workload_tag
        );
    }
    match &cap.clock {
        Some(c) => {
            let _ = writeln!(o, "clock: PPU timebase {} Hz; durations below are in timebase ticks", c.freq_hz);
        }
        None => {
            let _ = writeln!(o, "clock: unknown (no CLOCKS chunk)");
        }
    }

    let _ = writeln!(o, "\nlanes:");
    for (inc, lane) in &cap.lanes {
        let view = &a.lanes[inc];
        let name = lane.info.as_ref().map_or("", |i| i.name.as_str());
        let status = if view.valid { "MEASUREMENT VALID".to_string() } else { format!("INVALID ({})", view.reasons.join(", ")) };
        let _ = writeln!(o, "  {inc} {name}: {status}; {} records, {} frames", lane.written, lane.frames.len());
    }
    let valid: Vec<String> = a.lanes.values().filter(|l| l.valid).map(|l| l.incarnation.to_string()).collect();
    let _ = writeln!(
        o,
        "scope: statistics, frame elapsed and W use only lanes [{}]",
        if valid.is_empty() { "none".to_string() } else { valid.join(", ") }
    );

    let _ = writeln!(o, "\ndescriptors (as published):");
    for d in cap.descriptors.values() {
        let tag = if d.logical_tag.is_empty() { String::new() } else { format!(" [{}]", d.logical_tag) };
        let _ = writeln!(
            o,
            "  index {} id {:#018x} scope {} {}:{} {}{tag}",
            d.index, d.id, d.scope, d.file, d.line, d.name
        );
    }

    let _ = writeln!(o, "\nframe elapsed (FRAME k to FRAME k+1 of its owner):");
    for f in &a.frames {
        let _ = writeln!(o, "  owner {} frame {}: {}", f.owner, f.frame, f.value);
    }

    let _ = writeln!(o, "\nattributed work, per lane:\n  {W_NOTE}");
    for w in &a.work {
        let _ = writeln!(o, "  W(owner {}, frame {}, lane {}): {}", w.owner, w.frame, w.lane, w.value);
    }

    let _ = writeln!(o, "\nzones (valid lanes only):");
    for z in &a.zones {
        let tag = if z.logical_tag.is_empty() { String::new() } else { format!(" [{}]", z.logical_tag) };
        let _ = writeln!(o, "  {}{tag}: {} instances", z.name, z.instances);
        if let Some(label) = &z.label {
            let _ = writeln!(o, "    {label}");
        }
        let _ = writeln!(
            o,
            "    total {} | min {} | max {} | mean {} | p50 {} | p90 {} | p99 {}",
            z.total, z.min, z.max, z.mean, z.p50, z.p90, z.p99
        );
        if z.descriptors > 1 {
            let _ = writeln!(o, "    duplicate logical key ({} descriptors): A/B refused", z.descriptors);
        }
    }

    if !a.tokens.is_empty() {
        let _ = writeln!(o, "\ntokens:");
        for t in &a.tokens {
            let s = &cap.spans[t.span];
            let _ = writeln!(o, "  span {} of {}: frame {} of {}: {}", s.span_id, s.incarnation, t.frame, t.owner, t.outcome);
        }
    }

    let _ = writeln!(o, "\nspans:");
    for (s, v) in cap.spans.iter().zip(&a.spans) {
        let name = cap.descriptors.get(&s.desc).map_or("?", |d| d.name.as_str());
        let mut notes = Vec::new();
        if s.is_open() {
            notes.push("OPEN".to_string());
        }
        if s.truncated_at_stop {
            notes.push("TRUNCATED_AT_STOP".to_string());
        }
        if s.affected {
            notes.push("AFFECTED".to_string());
        }
        let dur = if s.is_censored() {
            "duration unknown (censored)".to_string()
        } else {
            match (s.inclusive(), s.exclusive()) {
                (Some(i), Some(x)) => format!("inclusive {i}, exclusive {x}"),
                (Some(i), None) => format!("inclusive {i}, exclusive unavailable (a child's duration is unavailable)"),
                (None, _) => "duration unavailable (CLOCK_ANOMALY: END before BEGIN)".to_string(),
            }
        };
        let _ = writeln!(
            o,
            "  {} span {} {name} depth {}: {dur}; {}; {}{}",
            s.incarnation,
            s.span_id,
            s.depth,
            v.attribution,
            v.spans_n,
            if notes.is_empty() { String::new() } else { format!("; {}", notes.join(", ")) }
        );
    }
    for line in extension_lines(cap) {
        let _ = writeln!(o, "{line}");
    }
    o
}

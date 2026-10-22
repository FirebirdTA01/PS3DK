//! The text report of `ps3dk-prof report`.  Refused and unknown values are printed as words, never as numbers.

use std::fmt::Write;

use crate::analysis::Analysis;
use crate::model::*;

pub const W_NOTE: &str = "W = start-frame-attributed EXCLUSIVE work of one lane: each span counts in the frame it \
started in, for its whole exclusive duration. W is not clipped to the frame interval, can exceed the frame's \
elapsed time, and is never summed across lanes.";

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
    let _ = writeln!(o, "verdict: {}", verdict_line(&a.verdict));
    if cap.minor_warning {
        let _ = writeln!(o, "warning: minor version > 0; read as v1");
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
        let dur = match (s.inclusive(), s.exclusive()) {
            (Some(i), Some(x)) if !s.is_censored() => format!("inclusive {i}, exclusive {x}"),
            _ => "duration unknown (censored)".to_string(),
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
    o
}

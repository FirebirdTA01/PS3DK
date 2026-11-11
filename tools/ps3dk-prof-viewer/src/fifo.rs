//! FIFO wrap waits (file_flags bit 6): document-owned bars on the PPU timebase axis.  The reader's pairing is the
//! authority: a COMPLETE phase is a bar with its elapsed bounds, an OPEN phase is a bar with an open end, and an
//! UNAVAILABLE phase is a visible UNKNOWN row, never a zero-length bar.  Nothing here is GPU busy or idle time.
use ps3dk_prof::{fifo as reader, Capture};

pub const TITLE: &str = "FIFO wrap waits (PPU clock)";
pub const CAPTION: &str = "PPU timebase wall time of the GCM context owner inside the two-phase FIFO wrap, including \
scheduling and observer cost. Not GPU busy or idle time; frame tokens are owner association, not GPU execution frames.";
pub const RSX_STATUS: &str = "RSX execution busy/idle: UNKNOWN (no mechanism in this capture measures it).";
pub const NOT_ENABLED: &str = "Not enabled in this capture (not zero): register samples, prepared/published bytes, \
RSX-reached markers, supply-gap views.";
pub const PAINT_BARS: usize = 4096;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bar {
    pub wrap_id: u64,
    /// 1 or 2
    pub phase: u8,
    pub start_tb: u64,
    /// None: OPEN, the end is UNKNOWN (drawn to the axis end with an open marker).
    pub end_tb: Option<u64>,
    pub min_ticks: Option<u64>,
    pub max_ticks: Option<u64>,
    pub token: Option<(u32, u64)>,
}
impl Bar {
    pub fn is_open(&self) -> bool {
        self.end_tb.is_none()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Row {
    pub wrap_id: u64,
    pub text: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FifoView {
    pub capability: bool,
    pub coverage: &'static str,
    pub refusal: Option<&'static str>,
    pub frequency: Option<u64>,
    pub bars: Vec<Bar>,
    /// Phases with no exact pairing: (wrap, phase, reason).
    pub unknown: Vec<(u64, u8, &'static str)>,
    pub rows: Vec<Row>,
    /// (first, last) PPU timebase of the drawn bars, the open ends included.
    pub axis: Option<(u64, u64)>,
    pub counts: String,
    pub config: String,
}

fn ticks(t: u64, f: Option<u64>) -> String {
    match f.filter(|&f| f != 0) {
        Some(f) => format!("{t} ticks (approx {:.3} ms)", t as f64 * 1000.0 / f as f64),
        None => format!("{t} ticks"),
    }
}

pub fn phase_text(p: &reader::Phase, f: Option<u64>) -> String {
    match p {
        reader::Phase::Complete { min, max, .. } => format!("COMPLETE {}..{}", ticks(*min, f), ticks(*max, f)),
        reader::Phase::Open { since_tb } => format!("OPEN since tb {since_tb}; end UNKNOWN"),
        reader::Phase::Unavailable(why) => format!("UNKNOWN ({why})"),
    }
}

impl FifoView {
    pub fn build(cap: &Capture) -> Self {
        let r = reader::analyze(cap);
        let mut v = FifoView { capability: r.capability, coverage: r.coverage, refusal: r.refusal, ..Self::default() };
        if !r.capability {
            return v;
        }
        let f = r.config.as_ref().map(|c| c.timebase_hz);
        v.frequency = f;
        if let Some(c) = &r.config {
            v.config = format!("instance {} generation {}, RSX domain {}, owner incarnation {}; ring {} records, \
                {} bytes allocated (ceiling {})", c.instance, c.generation, c.domain, c.owner_incarnation,
                c.ring_records, c.allocated_bytes, c.max_bytes);
        }
        v.counts = match &r.status {
            Some(s) => format!("wraps entered {} / completed {} (owner counts{}); visible WRAP_ENTER {} / WRAP_EXIT {}; \
                attempted {}, written {}, dropped {}, refused {}{}", s.wraps_started, s.wraps_completed,
                if s.exact() { "" } else { ", lower bounds" }, r.visible_enter, r.visible_exit, s.attempted,
                s.written, s.dropped, s.refused,
                if s.flags & reader::ST_OWNER_UNACK != 0 { "; owner unacknowledged: final totals UNAVAILABLE" }
                else { "" }),
            None => format!("visible WRAP_ENTER {} / WRAP_EXIT {}; no status: owner counts and loss UNKNOWN",
                r.visible_enter, r.visible_exit),
        };
        if r.refusal.is_some() {
            return v;
        }
        let right = r.last_tb;
        for w in &r.wraps {
            for (n, p) in [(1u8, &w.phase1), (2u8, &w.phase2)] {
                match p {
                    reader::Phase::Complete { min, max, begin_tb, end_tb } => v.bars.push(Bar {
                        wrap_id: w.wrap_id, phase: n, start_tb: *begin_tb, end_tb: Some(*end_tb),
                        min_ticks: Some(*min), max_ticks: Some(*max), token: w.token,
                    }),
                    reader::Phase::Open { since_tb } => v.bars.push(Bar {
                        wrap_id: w.wrap_id, phase: n, start_tb: *since_tb, end_tb: None, min_ticks: None,
                        max_ticks: None, token: w.token,
                    }),
                    reader::Phase::Unavailable(why) => v.unknown.push((w.wrap_id, n, why)),
                }
            }
            v.rows.push(Row {
                wrap_id: w.wrap_id,
                text: format!("wrap {} token {}: phase 1 {}; phase 2 {}; outer {}{}", w.wrap_id,
                    w.token.map_or("NONE".into(), |(o, fr)| format!("{o}/{fr}")), phase_text(&w.phase1, f),
                    phase_text(&w.phase2, f), phase_text(&w.outer, f),
                    if w.entered && !w.completed { " (entered, not completed)" } else { "" }),
            });
        }
        let lo = v.bars.iter().map(|b| b.start_tb).min();
        let hi = v.bars.iter().map(|b| b.end_tb.or(right).unwrap_or(b.start_tb)).max();
        v.axis = lo.zip(hi);
        v
    }

    pub fn status(&self) -> String {
        if !self.capability {
            "UNSUPPORTED: this capture has no FIFO wrap capability.".into()
        } else if let Some(why) = self.refusal {
            format!("REFUSED: {why}; no wrap rows analysed.")
        } else {
            format!("{}: {} bar(s), {} open, {} phase(s) UNKNOWN.", self.coverage, self.bars.len(),
                self.bars.iter().filter(|b| b.is_open()).count(), self.unknown.len())
        }
    }

    pub fn tooltip(&self, b: &Bar) -> String {
        let f = self.frequency;
        let span = match (b.min_ticks, b.max_ticks) {
            (Some(lo), Some(hi)) => format!("{}..{}", ticks(lo, f), ticks(hi, f)),
            _ => "OPEN: end UNKNOWN (at least since its begin)".into(),
        };
        format!("wrap {} phase {}: {span}. PPU wall time including scheduling and observer cost; not GPU time. \
            Token {} is owner association only.", b.wrap_id, b.phase,
            b.token.map_or("NONE".into(), |(o, fr)| format!("{o}/{fr}")))
    }
}

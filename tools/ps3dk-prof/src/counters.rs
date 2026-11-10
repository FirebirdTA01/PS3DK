//! Named per-frame counters (file_flags bit 8, chunk type 39 COUNTERS; docs/design/ps3dk-prof-format.md section 11).
//!
//! A counter sample is (incarnation, counter id, frame marker, timebase, signed 64-bit value).  The reader decodes
//! strictly: every violation is MALFORMED (Rule::Counters).  A capture without a TRAILER keeps the samples it has and
//! reports the inventory as not final; a capture WITH a TRAILER must carry exactly one FINAL counters chunk.
use std::collections::{BTreeMap, BTreeSet};
use std::io::{self, Write};

use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_COUNTERS: u32 = 256;
pub const CHUNK_COUNTERS: u32 = 39;
pub const SCHEMA: u16 = 1;
pub const FLAG_FINAL: u16 = 1;
pub const MAX_NAMES: usize = 64;
pub const MAX_NAME_LEN: usize = 64;
pub const SAMPLE_LEN: usize = 32;
pub const HEADER_LEN: usize = 32;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Sample {
    pub incarnation: u32,
    pub id: u32,
    /// The emitter's frame marker: 0 before its first FRAME, else 1 + the id of its last FRAME.
    pub frame: u64,
    pub tb: u64,
    pub value: i64,
}

/// The decoded counter inventory of one capture.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Inventory {
    /// Counter names by id (ids are dense, in declaration order).
    pub names: Vec<String>,
    pub samples: Vec<Sample>,
    /// Cumulative writer counts from the last chunk: samples lost to a full buffer, and refused calls.
    pub dropped: u64,
    pub refused: u64,
    /// The FINAL chunk was read (its counts are the capture's totals).
    pub final_seen: bool,
    pub chunks: u64,
}

/// The per-chunk decoder; `finish` applies the whole-file rules.
#[derive(Debug, Default)]
pub struct Reader {
    pub result: Inventory,
    seen: BTreeSet<String>,
}

impl Reader {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn chunk(&mut self, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        let inv = &mut self.result;
        if inv.final_seen {
            return malformed(Rule::Counters, "a COUNTERS chunk after the FINAL one");
        }
        let mut c = Cursor::new(payload, "COUNTERS");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let n_names = c.u32()?;
        let n_samples = c.u32()?;
        let reserved = c.u32()?;
        let dropped = c.u64()?;
        let refused = c.u64()?;
        if schema != SCHEMA {
            return malformed(Rule::Counters, format!("schema {schema}"));
        }
        if flags & !FLAG_FINAL != 0 || reserved != 0 {
            return malformed(Rule::Counters, format!("flags {flags:#x} reserved {reserved:#x}"));
        }
        if n_names == 0 && n_samples == 0 && flags & FLAG_FINAL == 0 {
            return malformed(Rule::Counters, "an empty non-final COUNTERS chunk");
        }
        if dropped < inv.dropped || refused < inv.refused {
            return malformed(Rule::Counters, format!(
                "cumulative counts decreased: dropped {} -> {dropped}, refused {} -> {refused}", inv.dropped, inv.refused));
        }
        if inv.names.len() + n_names as usize > MAX_NAMES {
            return malformed(Rule::Counters, format!("more than {MAX_NAMES} counter names"));
        }
        for _ in 0..n_names {
            let id = c.u32()?;
            let len = c.u16()? as usize;
            if id as usize != inv.names.len() {
                return malformed(Rule::Counters, format!("counter id {id} declared out of order (next is {})", inv.names.len()));
            }
            if len == 0 || len > MAX_NAME_LEN {
                return malformed(Rule::Counters, format!("counter {id}: name length {len}"));
            }
            let bytes = c.take(len)?;
            if !bytes.iter().all(|b| (0x21..=0x7e).contains(b)) {
                return malformed(Rule::Counters, format!("counter {id}: name is not printable ASCII without spaces"));
            }
            let name = String::from_utf8(bytes.to_vec()).expect("ASCII");
            if !self.seen.insert(name.clone()) {
                return malformed(Rule::Counters, format!("counter name {name:?} declared twice"));
            }
            inv.names.push(name);
        }
        let rest = c.remaining();
        if rest != n_samples as usize * SAMPLE_LEN {
            return malformed(Rule::Counters, format!("{rest} sample bytes for {n_samples} samples"));
        }
        for _ in 0..n_samples {
            let incarnation = c.u32()?;
            let id = c.u32()?;
            let frame = c.u64()?;
            let tb = c.u64()?;
            let value = c.u64()? as i64;
            if id as usize >= inv.names.len() {
                return malformed(Rule::Counters, format!("sample names undeclared counter {id}"));
            }
            if !cap.lanes.get(&incarnation).is_some_and(|l| l.info.is_some()) {
                return malformed(Rule::Counters, format!("sample names undeclared incarnation {incarnation}"));
            }
            inv.samples.push(Sample { incarnation, id, frame, tb, value });
        }
        c.done()?;
        inv.dropped = dropped;
        inv.refused = refused;
        inv.final_seen = flags & FLAG_FINAL != 0;
        inv.chunks += 1;
        Ok(())
    }

    /// `has_trailer`: the file ended with a TRAILER, so the writer sealed it and FINAL is required.
    pub fn finish(self, has_trailer: bool) -> Result<Inventory, Malformed> {
        if has_trailer && !self.result.final_seen {
            return malformed(Rule::Counters, "a sealed capture without a FINAL COUNTERS chunk");
        }
        Ok(self.result)
    }
}

/// One counter's summary over the capture.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Summary {
    pub name: String,
    pub samples: u64,
    pub frames: u64,
    pub min: i64,
    pub max: i64,
    pub last: i64,
    pub sum: i128,
}

/// Per counter, and per (incarnation, frame marker) the samples of that frame in order.
pub struct Report<'a> {
    pub inv: &'a Inventory,
    pub summaries: Vec<Summary>,
    pub frames: BTreeMap<(u32, u64), Vec<&'a Sample>>,
}

pub fn analyze(inv: &Inventory) -> Report<'_> {
    let mut frames: BTreeMap<(u32, u64), Vec<&Sample>> = BTreeMap::new();
    for s in &inv.samples {
        frames.entry((s.incarnation, s.frame)).or_default().push(s);
    }
    let summaries = inv.names.iter().enumerate().map(|(id, name)| {
        let mine: Vec<&Sample> = inv.samples.iter().filter(|s| s.id as usize == id).collect();
        let fr: BTreeSet<(u32, u64)> = mine.iter().map(|s| (s.incarnation, s.frame)).collect();
        Summary {
            name: name.clone(),
            samples: mine.len() as u64,
            frames: fr.len() as u64,
            min: mine.iter().map(|s| s.value).min().unwrap_or(0),
            max: mine.iter().map(|s| s.value).max().unwrap_or(0),
            last: mine.last().map_or(0, |s| s.value),
            sum: mine.iter().map(|s| s.value as i128).sum(),
        }
    }).collect();
    Report { inv, summaries, frames }
}

fn esc(s: &str) -> String {
    s.chars().map(|c| match c { '"' => "\\\"".into(), '\\' => "\\\\".into(), c => c.to_string() }).collect()
}

pub fn write_text(out: &mut dyn Write, cap: Option<&Inventory>) -> io::Result<()> {
    let Some(inv) = cap else {
        return writeln!(out, "counters: capability absent (file_flags bit 8 clear)");
    };
    let r = analyze(inv);
    writeln!(out, "counters: names={} samples={} frames={} dropped={} refused={} final={}", inv.names.len(),
             inv.samples.len(), r.frames.len(), inv.dropped, inv.refused, if inv.final_seen { "yes" } else { "no (not sealed)" })?;
    for s in &r.summaries {
        writeln!(out, "  {:<32} samples={} frames={} min={} max={} last={}", s.name, s.samples, s.frames, s.min, s.max, s.last)?;
    }
    for ((inc, frame), samples) in &r.frames {
        let label = if *frame == 0 { "before-first-frame".to_string() } else { format!("frame {}", frame - 1) };
        let vals: Vec<String> = samples.iter().map(|s| format!("{}={}", inv.names[s.id as usize], s.value)).collect();
        writeln!(out, "  incarnation {inc} {label}: {}", vals.join(" "))?;
    }
    Ok(())
}

pub fn write_json(out: &mut dyn Write, cap: Option<&Inventory>) -> io::Result<()> {
    let Some(inv) = cap else {
        return writeln!(out, "{{\"capability\":false}}");
    };
    let r = analyze(inv);
    write!(out, "{{\"capability\":true,\"final\":{},\"dropped\":{},\"refused\":{},\"names\":[", inv.final_seen, inv.dropped, inv.refused)?;
    for (i, s) in r.summaries.iter().enumerate() {
        write!(out, "{}{{\"name\":\"{}\",\"samples\":{},\"frames\":{},\"min\":{},\"max\":{},\"last\":{}}}",
               if i > 0 { "," } else { "" }, esc(&s.name), s.samples, s.frames, s.min, s.max, s.last)?;
    }
    write!(out, "],\"samples\":[")?;
    for (i, s) in inv.samples.iter().enumerate() {
        write!(out, "{}{{\"incarnation\":{},\"name\":\"{}\",\"frame\":{},\"tb\":{},\"value\":{}}}", if i > 0 { "," } else { "" },
               s.incarnation, esc(&inv.names[s.id as usize]), s.frame, s.tb, s.value)?;
    }
    writeln!(out, "]}}")
}

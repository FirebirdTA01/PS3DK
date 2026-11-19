//! RSX frame captures (file_flags bit 10, chunk types 44..48; docs/design/ps3dk-prof-format.md section 12).
//!
//! Two layers:
//! - the STRUCTURAL reader ([`Reader`], driven by `reader::read`): every rule of section 12 is MALFORMED
//!   (`Rule::RsxFrame`) when broken; the result is an [`Inventory`] of raw [`CaptureRecord`]s on `Capture::rsx_frame`;
//! - the ANALYSIS ([`frames`]): joins each draw with the AUTO operation that times it, resolves labels, programs and
//!   AUTO ranges (passes), and gives the viewer and the CLI one [`FrameView`] per capture.
//!
//! Timing is raw RSX report-timer ticks (the AUTO op's `raw_begin`/`raw_end`, as `ps3dk-prof auto --ops` prints them).
//! An interval is a reached-marker interval in PIPELINED captures and an isolated one in ISOLATED captures
//! ([`FrameView::isolated`]); the two are never mixed in one total.
use std::collections::{BTreeMap, HashMap};
use std::io::{self, Write};

use serde_json::{json, Value};

use crate::auto_rsx::{self, Op};
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_RSX_FRAME: u32 = 1024;
pub const CHUNK_CONFIG: u32 = 44;
pub const CHUNK_BEGIN: u32 = 45;
pub const CHUNK_DRAWS: u32 = 46;
pub const CHUNK_PROGRAMS: u32 = 47;
pub const CHUNK_END: u32 = 48;
pub const SCHEMA: u16 = 1;
pub const DECODER: u32 = 1;
pub const CONFIG_BYTES: usize = 64;
pub const BEGIN_FIXED: usize = 56;
pub const DRAWS_HEADER: usize = 16;
pub const END_BYTES: usize = 128;
pub const REC_DRAW: u16 = 1;
pub const REC_PROGRAM: u16 = 2;
pub const REC_LABEL: u16 = 3;
pub const REC_NAME: u16 = 4;
pub const DRAW_BYTES: usize = 96;
pub const PROGRAM_BYTES: usize = 48;
pub const NAME_MAX: usize = 63;
pub const NO_ORDINAL: u32 = u32::MAX;
/// Draw records kept per file; more are counted per capture ([`CaptureRecord::draws_not_retained`]).
pub const MAX_RETAINED_DRAWS: u64 = 1 << 20;
/// Captures per file.
pub const MAX_CAPTURES: usize = 256;

// draw flags
pub const DF_HOOKED: u16 = 1;
pub const DF_DECODED: u16 = 2;
pub const DF_MISMATCH: u16 = 4;
pub const DF_STATE_UNKNOWN: u16 = 8;
// untimed reasons beyond the AUTO exclusion columns 2..11
pub const UNTIMED_OUTSIDE_RANGE: u32 = 32;
pub const UNTIMED_RING_FULL: u32 = 33;
pub const UNTIMED_RING_STATE: u32 = 34;
pub const UNTIMED_NOT_HOOKED: u32 = 64;
pub const UNTIMED_NO_WINDOW: u32 = 65;

// ---- raw records ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Availability {
    Available,
    NoAuto,
    NoRing,
    NoDecoder,
}
impl Availability {
    fn of(v: u32) -> Option<Self> {
        Some(match v {
            0 => Availability::Available,
            1 => Availability::NoAuto,
            2 => Availability::NoRing,
            3 => Availability::NoDecoder,
            _ => return None,
        })
    }
    pub fn text(self) -> &'static str {
        match self {
            Availability::Available => "available",
            Availability::NoAuto => "unavailable: no AUTO attach",
            Availability::NoRing => "unavailable: the AUTO report ring is not enabled (PS3_PROF_AUTO_RING)",
            Availability::NoDecoder => "unavailable: the command-stream decoder could not start",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub decoder: u32,
    pub availability: Availability,
    pub block_bytes: u32,
    pub blocks: u32,
    pub program_capacity: u32,
    pub label_capacity: u32,
    pub label_bytes: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Live,
    App,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Begin {
    pub id: u32,
    pub source: Source,
    pub isolated: bool,
    pub program_bytes: bool,
    pub request_id: u32,
    pub owner: u32,
    pub frame: u64,
    pub window: u32,
    pub tb: u64,
    pub baseline_vp_start: Option<u32>,
    pub baseline_fp_address: Option<u32>,
    pub name: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum DrawKind {
    Array,
    Index,
    Clear,
    InlineArray,
    InlineIndex,
    Immediate,
}
impl DrawKind {
    fn of(v: u16) -> Option<Self> {
        Some(match v {
            1 => DrawKind::Array,
            2 => DrawKind::Index,
            3 => DrawKind::Clear,
            7 => DrawKind::InlineArray,
            8 => DrawKind::InlineIndex,
            9 => DrawKind::Immediate,
            _ => return None,
        })
    }
    pub fn name(self) -> &'static str {
        match self {
            DrawKind::Array => "ARRAY",
            DrawKind::Index => "INDEX",
            DrawKind::Clear => "CLEAR",
            DrawKind::InlineArray => "INLINE_ARRAY",
            DrawKind::InlineIndex => "INLINE_INDEX",
            DrawKind::Immediate => "IMMEDIATE",
        }
    }
    /// The AUTO operation kind that times this draw kind (None: AUTO has no kind for it yet).
    pub fn auto_kind(self) -> Option<u16> {
        match self {
            DrawKind::Array => Some(auto_rsx::KIND_ARRAY),
            DrawKind::Index => Some(auto_rsx::KIND_INDEX),
            DrawKind::Clear => Some(auto_rsx::KIND_CLEAR),
            _ => None,
        }
    }
}

/// The primitive name of a BEGIN_END mode (1..10).
pub fn primitive_name(mode: u32) -> &'static str {
    match mode {
        0 => "-",
        1 => "POINTS",
        2 => "LINES",
        3 => "LINE_LOOP",
        4 => "LINE_STRIP",
        5 => "TRIANGLES",
        6 => "TRIANGLE_STRIP",
        7 => "TRIANGLE_FAN",
        8 => "QUADS",
        9 => "QUAD_STRIP",
        10 => "POLYGON",
        _ => "?",
    }
}

/// Primitives a draw of `count` vertices or indices produces in `mode` (None for CLEAR or an unknown mode).
pub fn primitive_count(mode: u32, count: u32) -> Option<u64> {
    let n = u64::from(count);
    Some(match mode {
        1 => n,
        2 => n / 2,
        3 => if n >= 2 { n } else { 0 },
        4 => n.saturating_sub(1),
        5 => n / 3,
        6 | 7 => n.saturating_sub(2),
        8 => n / 4,
        9 => if n >= 4 { (n - 2) / 2 } else { 0 },
        10 => u64::from(n >= 3),
        _ => return None,
    })
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawDraw {
    pub ordinal: u32,
    pub kind: DrawKind,
    pub flags: u16,
    pub untimed: u32,
    pub op_id: u64,
    pub mode: u32,
    pub first: u32,
    pub count: u32,
    pub index_offset: u32,
    pub index_type: u8,
    pub index_location: u8,
    pub clear_mask: u32,
    pub tag: u32,
    pub label: u32,
    pub vp: u32,
    pub fp: u32,
    pub fp_address: u32,
    pub vp_start: u32,
    pub caller: u64,
    pub words: u32,
    pub tb: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Profile {
    Vertex,
    Fragment,
}
impl Profile {
    pub fn short(self) -> &'static str {
        match self {
            Profile::Vertex => "vp",
            Profile::Fragment => "fp",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProgramSource {
    Stream,
    Container,
    Memory,
    App,
}
impl ProgramSource {
    pub fn name(self) -> &'static str {
        match self {
            ProgramSource::Stream => "command stream",
            ProgramSource::Container => "program container at bind",
            ProgramSource::Memory => "RSX memory read",
            ProgramSource::App => "app registration",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Program {
    /// 1-based, as draws name it.
    pub index: u32,
    pub profile: Profile,
    pub source: ProgramSource,
    pub unterminated: bool,
    pub unreadable: bool,
    /// The identity comes from a bind made before the capture was armed: the container may have been rewritten in
    /// place since, so it is shown as "bound before capture, not revalidated" (a bind while armed is exact).
    pub not_revalidated: bool,
    pub instructions: u32,
    /// Canonical hash (format 12.6); 0 when unreadable.
    pub hash: u64,
    pub ucode_bytes: u32,
    pub const_blocks: u32,
    pub address: u32,
    pub first_draw: u32,
    /// The app-registered name (NAME record), if any.
    pub name: Option<String>,
}
impl Program {
    /// `vp:0123456789abcdef` (or `vp:unreadable`).
    pub fn id_text(&self) -> String {
        if self.unreadable {
            format!("{}:unreadable@{:#x}", self.profile.short(), self.address)
        } else {
            format!("{}:{:016x}", self.profile.short(), self.hash)
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Done,
    Truncated,
    Abandoned,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct End {
    pub outcome: Outcome,
    pub reason: u16,
    pub blocks: u32,
    pub blocks_dropped: u32,
    pub draws_seen: u32,
    pub draws_recorded: u32,
    pub hooked: u32,
    pub decoded_only: u32,
    pub mismatches: u32,
    pub truncated_at: Option<u32>,
    pub programs: u32,
    pub labels: u32,
    pub labels_refused: u32,
    pub programs_refused: u32,
    pub bytes_streamed: u64,
    pub bytes_dropped: u64,
    pub tb_end: u64,
    pub decoder_words: u64,
    /// (first rejected header, reason) when the decoder rejected anything.
    pub decoder_reject: Option<(u32, u32)>,
    /// Draw events the decoder could not hand to the capture (0 in this release: every event is recorded at once).
    pub lost_events: u32,
    pub untimed: u32,
}

/// END reason text.
pub fn end_reason(r: u16) -> &'static str {
    match r {
        0 => "none",
        1 => "staging full (the drainer was behind)",
        2 => "program table full",
        3 => "label table full",
        4 => "shutdown",
        5 => "context reset",
        6 => "command-stream decoder invalid",
        7 => "cancelled",
        _ => "?",
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CaptureRecord {
    pub begin: Begin,
    pub draws: Vec<RawDraw>,
    pub programs: Vec<Program>,
    pub labels: Vec<String>,
    pub block_seqs: Vec<u32>,
    pub end: Option<End>,
    /// DRAW records parsed but not kept (the file's retention bound).
    pub draws_not_retained: u64,
    /// Text fields (name, labels, program names) that were not valid UTF-8 and are shown with U+FFFD replacements.
    pub text_replaced: u32,
}

/// The decoded family of one file: present exactly when file_flags bit 10 is.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Inventory {
    pub config: Option<Config>,
    pub captures: Vec<CaptureRecord>,
}

// ---- structural reader ----------------------------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct Reader {
    pub result: Inventory,
    open: bool,
    retained: u64,
    last_ordinal: Option<u32>,
    by_hash: BTreeMap<(Profile, u64, bool), u32>,
    named: Vec<bool>,
}

fn bad<T>(msg: impl Into<String>) -> Result<T, Malformed> {
    malformed(Rule::RsxFrame, msg.into())
}

/// A capture's text field (BEGIN name, LABEL, NAME). Invalid UTF-8 or a NUL degrades THAT field to U+FFFD
/// replacements (counted in `replaced`); the file stays readable. The runtime writes valid UTF-8 only.
fn text(bytes: &[u8], replaced: &mut u32) -> String {
    match std::str::from_utf8(bytes) {
        Ok(t) if !t.contains('\0') => t.to_string(),
        _ => {
            *replaced += 1;
            String::from_utf8_lossy(bytes).replace('\0', "\u{fffd}")
        }
    }
}

fn zeros(c: &mut Cursor<'_>, n: usize, what: &str) -> Result<(), Malformed> {
    if c.take(n)?.iter().any(|&b| b != 0) {
        return bad(format!("{what}: nonzero reserved or padding bytes"));
    }
    Ok(())
}

fn pad8(n: usize) -> usize {
    (n + 7) & !7
}

impl Reader {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn chunk(&mut self, ty: u32, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        if ty != CHUNK_CONFIG && self.result.config.is_none() {
            return bad(format!("chunk type {ty} before RSXF_CONFIG"));
        }
        match ty {
            CHUNK_CONFIG => self.config(payload),
            CHUNK_BEGIN => self.begin(payload, cap),
            CHUNK_DRAWS => self.draws(payload),
            CHUNK_END => self.end(payload),
            CHUNK_PROGRAMS => bad("RSXF_PROGRAMS in a schema 1 capture"),
            _ => bad(format!("chunk type {ty}")),
        }
    }

    fn config(&mut self, p: &[u8]) -> Result<(), Malformed> {
        if self.result.config.is_some() {
            return bad("a second RSXF_CONFIG");
        }
        if p.len() != CONFIG_BYTES {
            return bad(format!("RSXF_CONFIG is {} bytes, expected {CONFIG_BYTES}", p.len()));
        }
        let mut c = Cursor::new(p, "RSXF_CONFIG");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let decoder = c.u32()?;
        let avail = c.u32()?;
        let block_bytes = c.u32()?;
        let blocks = c.u32()?;
        let program_capacity = c.u32()?;
        let label_capacity = c.u32()?;
        let label_bytes = c.u32()?;
        zeros(&mut c, 32, "RSXF_CONFIG")?;
        if schema != SCHEMA {
            return bad(format!("RSXF_CONFIG schema {schema}"));
        }
        if flags != 0 || decoder != DECODER {
            return bad(format!("RSXF_CONFIG flags {flags:#x} decoder {decoder}"));
        }
        let Some(availability) = Availability::of(avail) else {
            return bad(format!("RSXF_CONFIG availability {avail}"));
        };
        if !(4096..=1 << 20).contains(&block_bytes) || block_bytes % 8 != 0 || !(1..=16).contains(&blocks)
            || !(1..=65535).contains(&program_capacity) || !(1..=65535).contains(&label_capacity)
            || !(1..=255).contains(&label_bytes)
        {
            return bad(format!(
                "RSXF_CONFIG limits: block {block_bytes} x {blocks}, programs {program_capacity}, labels \
                 {label_capacity} x {label_bytes}"
            ));
        }
        self.result.config = Some(Config {
            decoder,
            availability,
            block_bytes,
            blocks,
            program_capacity,
            label_capacity,
            label_bytes,
        });
        Ok(())
    }

    fn begin(&mut self, p: &[u8], cap: &Capture) -> Result<(), Malformed> {
        let cfg = self.result.config.as_ref().expect("checked");
        if cfg.availability != Availability::Available {
            return bad(format!("RSXF_BEGIN although the capture is {}", cfg.availability.text()));
        }
        if self.open {
            return bad("RSXF_BEGIN while another capture is open");
        }
        if self.result.captures.len() >= MAX_CAPTURES {
            return bad(format!("more than {MAX_CAPTURES} RSX frame captures"));
        }
        let mut c = Cursor::new(p, "RSXF_BEGIN");
        let id = c.u32()?;
        let source = c.u16()?;
        let flags = c.u16()?;
        let request_id = c.u32()?;
        let owner = c.u32()?;
        let frame = c.u64()?;
        let window = c.u32()?;
        let r0 = c.u32()?;
        let tb = c.u64()?;
        let vp = c.u32()?;
        let fp = c.u32()?;
        let known = c.u32()?;
        let n = c.u16()? as usize;
        let r1 = c.u16()?;
        if p.len() != BEGIN_FIXED + pad8(n) {
            return bad(format!("RSXF_BEGIN is {} bytes for a {n}-byte name", p.len()));
        }
        let mut replaced = 0;
        let name = text(c.take(n)?, &mut replaced);
        zeros(&mut c, pad8(n) - n, "RSXF_BEGIN")?;
        let prev = self.result.captures.last().map_or(0, |x| x.begin.id);
        if id == 0 || id <= prev {
            return bad(format!("RSXF_BEGIN capture id {id} after {prev}"));
        }
        let source = match source {
            1 => Source::Live,
            2 => Source::App,
            s => return bad(format!("RSXF_BEGIN source {s}")),
        };
        if flags & !3 != 0 || known & !3 != 0 || r0 != 0 || r1 != 0 || n > NAME_MAX {
            return bad(format!("RSXF_BEGIN flags {flags:#x} known {known:#x} name length {n}"));
        }
        if (source == Source::Live) != (request_id != 0) {
            return bad(format!("RSXF_BEGIN request id {request_id} for a {source:?} request"));
        }
        if !cap.lanes.get(&owner).is_some_and(|l| l.info.is_some()) {
            return bad(format!("RSXF_BEGIN names undeclared owner incarnation {owner}"));
        }
        if (known & 1 == 0 && vp != 0) || (known & 2 == 0 && fp != 0) {
            return bad("RSXF_BEGIN baseline value without its known bit");
        }
        self.result.captures.push(CaptureRecord {
            begin: Begin {
                id,
                source,
                isolated: flags & 1 != 0,
                program_bytes: flags & 2 != 0,
                request_id,
                owner,
                frame,
                window,
                tb,
                baseline_vp_start: (known & 1 != 0).then_some(vp),
                baseline_fp_address: (known & 2 != 0).then_some(fp),
                name,
            },
            draws: Vec::new(),
            programs: Vec::new(),
            labels: Vec::new(),
            block_seqs: Vec::new(),
            end: None,
            draws_not_retained: 0,
            text_replaced: replaced,
        });
        self.open = true;
        self.last_ordinal = None;
        self.by_hash.clear();
        self.named.clear();
        Ok(())
    }

    fn draws(&mut self, p: &[u8]) -> Result<(), Malformed> {
        if !self.open {
            return bad("RSXF_DRAWS without an open capture");
        }
        let label_bytes = self.result.config.as_ref().expect("checked").label_bytes as usize;
        let mut c = Cursor::new(p, "RSXF_DRAWS");
        let id = c.u32()?;
        let seq = c.u32()?;
        let n = c.u32()?;
        let bytes = c.u32()? as usize;
        let rec = self.result.captures.last_mut().expect("open");
        if id != rec.begin.id {
            return bad(format!("RSXF_DRAWS for capture {id} while capture {} is open", rec.begin.id));
        }
        if rec.block_seqs.last().is_some_and(|&s| seq <= s) {
            return bad(format!("RSXF_DRAWS block sequence {seq} after {}", rec.block_seqs.last().unwrap()));
        }
        if bytes != p.len() - DRAWS_HEADER.min(p.len()) || n == 0 {
            return bad(format!("RSXF_DRAWS: {n} records in {bytes} bytes, payload {}", p.len()));
        }
        rec.block_seqs.push(seq);
        for k in 0..n {
            if c.remaining() < 4 {
                return bad(format!("RSXF_DRAWS: record {k} of {n} past the record bytes"));
            }
            let ty = c.u16()?;
            let len = c.u16()? as usize;
            if len < 8 || len % 8 != 0 || len - 4 > c.remaining() {
                return bad(format!("record type {ty} length {len}"));
            }
            match ty {
                REC_DRAW => {
                    if len != DRAW_BYTES {
                        return bad(format!("DRAW record length {len}"));
                    }
                    let ordinal = c.u32()?;
                    let kind = c.u16()?;
                    let flags = c.u16()?;
                    let untimed = c.u32()?;
                    let op_id = c.u64()?;
                    let mode = c.u32()?;
                    let first = c.u32()?;
                    let count = c.u32()?;
                    let index_offset = c.u32()?;
                    let index_type = c.u8()?;
                    let index_location = c.u8()?;
                    let r0 = c.u16()?;
                    let clear_mask = c.u32()?;
                    let tag = c.u32()?;
                    let label = c.u32()?;
                    let vp = c.u32()?;
                    let fp = c.u32()?;
                    let fp_address = c.u32()?;
                    let vp_start = c.u32()?;
                    let caller = c.u64()?;
                    let words = c.u32()?;
                    let r1 = c.u32()?;
                    let tb = c.u64()?;
                    let Some(kind) = DrawKind::of(kind) else { return bad(format!("draw {ordinal}: kind {kind}")) };
                    if self.last_ordinal.is_some_and(|o| ordinal <= o) {
                        return bad(format!("draw ordinal {ordinal} after {}", self.last_ordinal.unwrap()));
                    }
                    self.last_ordinal = Some(ordinal);
                    if flags & !15 != 0 || flags & (DF_HOOKED | DF_DECODED) == 0 || r0 != 0 || r1 != 0 {
                        return bad(format!("draw {ordinal}: flags {flags:#x}"));
                    }
                    if (untimed == 0) != (op_id != 0) {
                        return bad(format!("draw {ordinal}: untimed reason {untimed} with op id {op_id}"));
                    }
                    if untimed != 0 && !reason_known(untimed) {
                        return bad(format!("draw {ordinal}: untimed reason {untimed}"));
                    }
                    if op_id != 0 && (flags & DF_HOOKED == 0 || kind.auto_kind().is_none()) {
                        return bad(format!("draw {ordinal}: an op id on a draw AUTO cannot time"));
                    }
                    let indexed = matches!(kind, DrawKind::Index | DrawKind::InlineIndex);
                    if index_type > 1 || index_location > 1 || (!indexed && (index_type | index_location) != 0)
                        || (kind != DrawKind::Index && index_offset != 0)
                        || (kind == DrawKind::Clear) != (mode == 0) || mode > 10
                        || (kind != DrawKind::Clear && clear_mask != 0)
                    {
                        return bad(format!("draw {ordinal}: arguments inconsistent with {}", kind.name()));
                    }
                    if label as usize > rec.labels.len() {
                        return bad(format!("draw {ordinal}: undefined label {label}"));
                    }
                    for (which, idx, want) in [("VP", vp, Profile::Vertex), ("FP", fp, Profile::Fragment)] {
                        if idx != 0 {
                            match rec.programs.get(idx as usize - 1) {
                                Some(p) if p.profile == want => {}
                                Some(_) => return bad(format!("draw {ordinal}: {which} index {idx} names a program of the other profile")),
                                None => return bad(format!("draw {ordinal}: undefined {which} program {idx}")),
                            }
                        }
                    }
                    if flags & DF_HOOKED == 0 && caller != 0 {
                        return bad(format!("draw {ordinal}: caller without a hook"));
                    }
                    if self.retained >= MAX_RETAINED_DRAWS {
                        rec.draws_not_retained += 1;
                        continue;
                    }
                    self.retained += 1;
                    rec.draws.push(RawDraw {
                        ordinal, kind, flags, untimed, op_id, mode, first, count, index_offset, index_type,
                        index_location, clear_mask, tag, label, vp, fp, fp_address, vp_start, caller, words, tb,
                    });
                }
                REC_PROGRAM => {
                    if len != PROGRAM_BYTES {
                        return bad(format!("PROGRAM record length {len}"));
                    }
                    let index = c.u32()?;
                    let profile = c.u8()?;
                    let source = c.u8()?;
                    let flags = c.u16()?;
                    let instructions = c.u32()?;
                    let hash = c.u64()?;
                    let ucode_bytes = c.u32()?;
                    let const_blocks = c.u32()?;
                    let address = c.u32()?;
                    let first_draw = c.u32()?;
                    let r = c.u64()?;
                    if index as usize != rec.programs.len() + 1 {
                        return bad(format!("program index {index} defined out of order"));
                    }
                    let profile = match profile {
                        1 => Profile::Vertex,
                        2 => Profile::Fragment,
                        v => return bad(format!("program {index}: profile {v}")),
                    };
                    let source = match source {
                        1 => ProgramSource::Stream,
                        2 => ProgramSource::Container,
                        3 => ProgramSource::Memory,
                        4 => ProgramSource::App,
                        v => return bad(format!("program {index}: source {v}")),
                    };
                    let unreadable = flags & 2 != 0;
                    let not_revalidated = flags & 4 != 0;
                    if flags & !7 != 0 || r != 0 || unreadable != (hash == 0)
                        || (profile == Profile::Vertex && const_blocks != 0)
                    {
                        return bad(format!("program {index}: flags {flags:#x} hash {hash:#x}"));
                    }
                    if !unreadable && self.by_hash.insert((profile, hash, not_revalidated), index).is_some() {
                        return bad(format!("program {index}: {} defined twice", profile.short()));
                    }
                    rec.programs.push(Program {
                        index, profile, source, unterminated: flags & 1 != 0, unreadable, not_revalidated, instructions, hash,
                        ucode_bytes, const_blocks, address, first_draw, name: None,
                    });
                    self.named.push(false);
                }
                REC_LABEL | REC_NAME => {
                    let index = c.u32()?;
                    let n = c.u16()? as usize;
                    let r0 = c.u16()?;
                    let r1 = c.u32()?;
                    if len != 16 + pad8(n) || n == 0 || n > label_bytes || r0 != 0 || r1 != 0 {
                        return bad(format!("record type {ty}: length {len} text {n} bytes"));
                    }
                    let s = text(c.take(n)?, &mut rec.text_replaced);
                    zeros(&mut c, pad8(n) - n, "label")?;
                    if ty == REC_LABEL {
                        if index as usize != rec.labels.len() + 1 {
                            return bad(format!("label index {index} defined out of order"));
                        }
                        rec.labels.push(s);
                    } else {
                        let i = index as usize;
                        if i == 0 || i > rec.programs.len() || self.named[i - 1] {
                            return bad(format!("NAME for program {index}"));
                        }
                        self.named[i - 1] = true;
                        rec.programs[i - 1].name = Some(s);
                    }
                }
                _ => return bad(format!("record type {ty}")),
            }
        }
        if c.remaining() != 0 {
            return bad("RSXF_DRAWS: bytes after the last record");
        }
        Ok(())
    }

    fn end(&mut self, p: &[u8]) -> Result<(), Malformed> {
        if !self.open {
            return bad("RSXF_END without an open capture");
        }
        if p.len() != END_BYTES {
            return bad(format!("RSXF_END is {} bytes", p.len()));
        }
        let mut c = Cursor::new(p, "RSXF_END");
        let id = c.u32()?;
        let outcome = c.u16()?;
        let reason = c.u16()?;
        let v: Vec<u32> = (0..12).map(|_| c.u32()).collect::<Result<_, _>>()?;
        let (blocks, blocks_dropped, draws_seen, draws_recorded, hooked, decoded_only, mismatches, truncated_at,
            programs, labels, labels_refused, programs_refused) =
            (v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
        let bytes_streamed = c.u64()?;
        let bytes_dropped = c.u64()?;
        let tb_end = c.u64()?;
        let decoder_words = c.u64()?;
        let rej_header = c.u32()?;
        let rej_reason = c.u32()?;
        let lost_events = c.u32()?;
        let r0 = c.u32()?;
        let r3 = c.u64()?;
        let untimed = c.u32()?;
        let r1 = c.u32()?;
        let r2 = c.u64()?;
        let rec = self.result.captures.last_mut().expect("open");
        if id != rec.begin.id {
            return bad(format!("RSXF_END for capture {id} while capture {} is open", rec.begin.id));
        }
        let outcome = match outcome {
            1 => Outcome::Done,
            2 => Outcome::Truncated,
            3 => Outcome::Abandoned,
            o => return bad(format!("RSXF_END outcome {o}")),
        };
        if reason > 7 || r0 != 0 || r1 != 0 || r2 != 0 || r3 != 0 || (rej_reason == 0 && rej_header != 0) {
            return bad(format!("RSXF_END reason {reason} or reserved fields"));
        }
        let recorded = rec.draws.len() as u64 + rec.draws_not_retained;
        if blocks as usize != rec.block_seqs.len() {
            return bad(format!("RSXF_END: {blocks} blocks emitted, {} seen", rec.block_seqs.len()));
        }
        if rec.block_seqs.last().is_some_and(|&s| u64::from(s) >= u64::from(blocks) + u64::from(blocks_dropped)) {
            return bad("RSXF_END: a block sequence beyond emitted + dropped");
        }
        if u64::from(draws_recorded) != recorded || draws_recorded > draws_seen
            || hooked.saturating_add(decoded_only) > draws_seen || untimed > draws_seen
        {
            return bad(format!("RSXF_END draw counts: seen {draws_seen} recorded {draws_recorded} (records {recorded})"));
        }
        if programs as usize != rec.programs.len() || labels as usize != rec.labels.len() {
            return bad(format!("RSXF_END: {programs} programs and {labels} labels against the records"));
        }
        let truncated = truncated_at != NO_ORDINAL;
        if truncated != (outcome == Outcome::Truncated) {
            return bad(format!("RSXF_END outcome {outcome:?} with truncated_at {truncated_at:#x}"));
        }
        if truncated && rec.draws.last().is_some_and(|d| d.ordinal >= truncated_at) {
            return bad("RSXF_END: a recorded draw at or after the truncation ordinal");
        }
        rec.end = Some(End {
            outcome, reason, blocks, blocks_dropped, draws_seen, draws_recorded, hooked, decoded_only, mismatches,
            truncated_at: truncated.then_some(truncated_at), programs, labels, labels_refused, programs_refused,
            bytes_streamed, bytes_dropped, tb_end, decoder_words,
            decoder_reject: (rej_reason != 0).then_some((rej_header, rej_reason)), lost_events, untimed,
        });
        self.open = false;
        Ok(())
    }

    /// Whole-file rules: CONFIG present when the file has a TRAILER; no capture left open behind a TRAILER.
    pub fn finish(self, trailer_seen: bool) -> Result<Inventory, Malformed> {
        if trailer_seen && self.result.config.is_none() {
            return bad("file_flags bit 10 without an RSXF_CONFIG chunk");
        }
        if trailer_seen && self.open {
            return bad("an RSX frame capture without its RSXF_END before the TRAILER");
        }
        Ok(self.result)
    }
}

fn reason_known(r: u32) -> bool {
    (2..=11).contains(&r)
        || matches!(r, UNTIMED_OUTSIDE_RANGE | UNTIMED_RING_FULL | UNTIMED_RING_STATE | UNTIMED_NOT_HOOKED | UNTIMED_NO_WINDOW)
}

/// The text of a draw's untimed reason code.
pub fn untimed_text(r: u32) -> &'static str {
    match r {
        0 => "timed",
        2..=11 => auto_rsx::EXCLUSIONS[(r - 2) as usize],
        UNTIMED_OUTSIDE_RANGE => "outside timed range",
        UNTIMED_RING_FULL => "report ring full",
        UNTIMED_RING_STATE => "report ring state",
        UNTIMED_NOT_HOOKED => "not hooked (raw or unsafe writer)",
        UNTIMED_NO_WINDOW => "no capture window",
        _ => "?",
    }
}

// ---- canonical hash and program containers --------------------------------------------------------------------------

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;
pub const VP_DOMAIN: &[u8] = b"ps3dk.rsxf.vp1";
pub const FP_DOMAIN: &[u8] = b"ps3dk.rsxf.fp1";
pub const VP_MAX_INSTRUCTIONS: usize = 512;
pub const FP_MAX_INSTRUCTIONS: usize = 65536;

fn fnv(h: u64, bytes: &[u8]) -> u64 {
    bytes.iter().fold(h, |h, &b| (h ^ u64::from(b)).wrapping_mul(FNV_PRIME))
}

/// One program's canonical identity (format 12.6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProgramHash {
    pub profile: Profile,
    pub hash: u64,
    pub instructions: u32,
    /// Bytes walked, instructions and constant blocks.
    pub bytes: u32,
    pub const_blocks: u32,
    /// No end bit was found within the bytes or the bound.
    pub unterminated: bool,
}

fn word(b: &[u8], i: usize) -> u32 {
    u32::from_be_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

/// The VP hash of `ucode` (big-endian bytes from the start slot).
pub fn vp_hash(ucode: &[u8]) -> ProgramHash {
    let mut h = fnv(FNV_OFFSET, VP_DOMAIN);
    let mut n = 0usize;
    let mut unterminated = true;
    while (n + 1) * 16 <= ucode.len() && n < VP_MAX_INSTRUCTIONS {
        let inst = &ucode[n * 16..n * 16 + 16];
        h = fnv(h, inst);
        n += 1;
        if word(inst, 12) & 1 != 0 {
            unterminated = false;
            break;
        }
    }
    ProgramHash { profile: Profile::Vertex, hash: h, instructions: n as u32, bytes: (n * 16) as u32, const_blocks: 0, unterminated }
}

/// The FP hash of `ucode` (the bytes as they lie in memory, halfword-swapped words).
pub fn fp_hash(ucode: &[u8]) -> ProgramHash {
    let swap = |w: u32| w.rotate_left(16);
    let mut h = fnv(FNV_OFFSET, FP_DOMAIN);
    let (mut pos, mut n, mut consts) = (0usize, 0u32, 0u32);
    let mut unterminated = true;
    while pos + 16 <= ucode.len() && (n as usize) < FP_MAX_INSTRUCTIONS {
        let inst = &ucode[pos..pos + 16];
        h = fnv(h, inst);
        n += 1;
        pos += 16;
        let end = swap(word(inst, 0)) & 1 != 0;
        if (1..4).any(|k| swap(word(inst, 4 * k)) & 3 == 2) {
            consts += 1;
            pos = (pos + 16).min(ucode.len());
        }
        if end {
            unterminated = false;
            break;
        }
    }
    ProgramHash { profile: Profile::Fragment, hash: h, instructions: n, bytes: pos as u32, const_blocks: consts, unterminated }
}

pub const CG_PROFILE_VP: u32 = 7003;
pub const CG_PROFILE_FP: u32 = 7004;

/// The canonical hash of a program container (the Cg binary container: big-endian header, profile 7003 VP or 7004
/// FP, ucode size at 24 and offset at 28).
pub fn container_hash(bytes: &[u8]) -> Result<ProgramHash, String> {
    if bytes.len() < 32 {
        return Err(format!("{} bytes: too short for a program container", bytes.len()));
    }
    let profile = word(bytes, 0);
    let size = word(bytes, 24) as usize;
    let off = word(bytes, 28) as usize;
    let Some(ucode) = off.checked_add(size).and_then(|e| bytes.get(off..e)) else {
        return Err(format!("ucode [{off:#x}, +{size:#x}) outside the {}-byte container", bytes.len()));
    };
    match profile {
        CG_PROFILE_VP => Ok(vp_hash(ucode)),
        CG_PROFILE_FP => Ok(fp_hash(ucode)),
        p => Err(format!("profile {p} is neither a vertex (7003) nor a fragment (7004) program")),
    }
}

// ---- analysis -------------------------------------------------------------------------------------------------------

/// How a draw's time is known.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Timing {
    /// Raw RSX report-timer values of the AUTO operation's begin and end markers.
    Timed { raw_begin: u64, raw_end: u64 },
    /// The runtime could not time it (the code's text: [`untimed_text`]).
    Untimed(u32),
    /// The AUTO record is not in the file (not drained yet, or not retained by the AUTO reader).
    Missing,
    /// The AUTO record exists but carries no usable interval (the reason).
    Invalid(&'static str),
}
impl Timing {
    pub fn interval(&self) -> Option<u64> {
        match self {
            Timing::Timed { raw_begin, raw_end } => Some(raw_end - raw_begin),
            _ => None,
        }
    }
    pub fn text(&self) -> String {
        match self {
            Timing::Timed { raw_begin, raw_end } => format!("{}", raw_end - raw_begin),
            Timing::Untimed(r) => format!("untimed: {}", untimed_text(*r)),
            Timing::Missing => "timing record absent".into(),
            Timing::Invalid(why) => format!("timing invalid: {why}"),
        }
    }
}

/// One draw, resolved.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Draw {
    pub raw: RawDraw,
    /// The label path (None: no label).
    pub label: Option<String>,
    /// Indices into [`FrameView::programs`].
    pub vp: Option<usize>,
    pub fp: Option<usize>,
    pub timing: Timing,
    /// RSX ticks from the latest earlier timed end (submission order) to this begin; 0 when they overlap.
    pub gap: Option<u64>,
    /// Index into [`FrameView::passes`]: the innermost AUTO range containing this draw's begin.
    pub pass: Option<usize>,
    /// The AUTO op kind disagrees with the draw kind (a mismatch the reader found).
    pub op_mismatch: bool,
}
impl Draw {
    pub fn hooked(&self) -> bool {
        self.raw.flags & DF_HOOKED != 0
    }
    pub fn decoded(&self) -> bool {
        self.raw.flags & DF_DECODED != 0
    }
    pub fn primitives(&self) -> Option<u64> {
        if self.raw.kind == DrawKind::Clear { None } else { primitive_count(self.raw.mode, self.raw.count) }
    }
}

/// One AUTO range of the captured frame (a pass).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pass {
    pub name: String,
    pub depth: u32,
    pub tag: Option<u32>,
    pub raw_begin: u64,
    pub raw_end: u64,
    /// Draws whose begin falls in it as their innermost range.
    pub draws: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FrameStatus {
    /// END DONE and every timed draw's AUTO record present.
    Complete,
    /// END TRUNCATED: draws from `at` on carry no record (time only in AUTO, if any).
    Truncated { at: u32, reason: u16 },
    Abandoned { reason: u16 },
    /// No END yet (a live prefix or a file without a trailer).
    Open,
    /// END DONE but `missing` timed draws have no AUTO record yet.
    PendingTiming { missing: u32 },
}
impl FrameStatus {
    pub fn text(&self) -> String {
        match self {
            FrameStatus::Complete => "COMPLETE".into(),
            FrameStatus::Truncated { at, reason } => format!("TRUNCATED at draw {at} ({})", end_reason(*reason)),
            FrameStatus::Abandoned { reason } => format!("ABANDONED ({})", end_reason(*reason)),
            FrameStatus::Open => "OPEN (no RSXF_END yet)".into(),
            FrameStatus::PendingTiming { missing } => format!("PENDING TIMING ({missing} AUTO records not yet present)"),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Totals {
    pub draws: u32,
    pub timed: u32,
    pub untimed: u32,
    pub missing: u32,
    /// Draws whose AUTO operation is of another kind or frame: neither timed nor counted in any sum.
    pub mismatched: u32,
    /// Sum of timed intervals (raw ticks): in a PIPELINED capture overlapping intervals are each counted.
    pub sum: u64,
    /// First timed begin .. last timed end.
    pub span: Option<(u64, u64)>,
    pub gaps: u64,
    pub primitives: u64,
}

/// One RSX frame capture, resolved for display.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameView {
    pub id: u32,
    pub name: String,
    pub source: Source,
    pub request_id: u32,
    pub owner: u32,
    pub frame: u64,
    pub window: u32,
    pub isolated: bool,
    pub status: FrameStatus,
    pub draws: Vec<Draw>,
    pub programs: Vec<Program>,
    pub passes: Vec<Pass>,
    pub totals: Totals,
    pub end: Option<End>,
    pub draws_not_retained: u64,
    pub text_replaced: u32,
}

fn op_interval(o: &Op) -> Result<(u64, u64), &'static str> {
    if !o.raw_valid() {
        return Err("raw timestamps not valid");
    }
    if o.flags & auto_rsx::OP_CLOCK_INVALID != 0 {
        return Err("clock invalid");
    }
    if o.raw_end < o.raw_begin {
        return Err("end before begin");
    }
    Ok((o.raw_begin, o.raw_end))
}

/// Every RSX frame capture of `cap`, in capture order.
pub fn frames(cap: &Capture) -> Vec<FrameView> {
    let Some(inv) = &cap.rsx_frame else { return Vec::new() };
    let ops: HashMap<u64, &Op> = cap.auto.as_ref().map_or_else(HashMap::new, |a| a.ops.iter().map(|o| (o.id, o)).collect());
    inv.captures.iter().map(|c| view(cap, c, &ops)).collect()
}

fn view(cap: &Capture, c: &CaptureRecord, ops: &HashMap<u64, &Op>) -> FrameView {
    let b = &c.begin;
    // passes: the frame's AUTO ranges
    let mut passes: Vec<Pass> = cap.auto.as_ref().map_or_else(Vec::new, |a| {
        a.ops.iter()
            .filter(|o| o.kind == auto_rsx::KIND_RANGE && o.token() == Some((b.owner, b.frame)))
            .filter_map(|o| op_interval(o).ok().map(|(rb, re)| (o, rb, re)))
            .map(|(o, rb, re)| Pass {
                name: cap.descriptors.get(&o.args[0].wrapping_sub(1))
                    .map_or_else(|| format!("descriptor {}", o.args[0].wrapping_sub(1)), |d| d.name.clone()),
                depth: o.depth,
                tag: o.tag,
                raw_begin: rb,
                raw_end: re,
                draws: 0,
            })
            .collect()
    });
    passes.sort_by_key(|p| (p.raw_begin, p.depth));
    let mut draws = Vec::with_capacity(c.draws.len());
    let mut totals = Totals::default();
    let mut last_end: Option<u64> = None;
    for d in &c.draws {
        let mut op_mismatch = false;
        let flagged = d.flags & DF_MISMATCH != 0;
        let timing = if d.op_id == 0 {
            Timing::Untimed(d.untimed)
        } else {
            match ops.get(&d.op_id) {
                None => Timing::Missing,
                Some(o) => {
                    op_mismatch = d.kind.auto_kind() != Some(o.kind) || o.token() != Some((b.owner, b.frame));
                    if op_mismatch {
                        // the op is not this draw's (another kind or frame): its interval is not the draw's time
                        Timing::Invalid("AUTO operation of another kind or frame")
                    } else if flagged {
                        // the runtime saw the hooked call and the decoded words disagree: not this draw's time either
                        Timing::Invalid("hooked call and decoded draw disagree")
                    } else {
                        match op_interval(o) {
                            Ok((rb, re)) => Timing::Timed { raw_begin: rb, raw_end: re },
                            Err(why) => Timing::Invalid(why),
                        }
                    }
                }
            }
        };
        let mut gap = None;
        let mut pass = None;
        totals.draws += 1;
        match &timing {
            Timing::Timed { raw_begin, raw_end } => {
                totals.timed += 1;
                totals.sum += raw_end - raw_begin;
                totals.span = Some(totals.span.map_or((*raw_begin, *raw_end), |(s, e)| (s.min(*raw_begin), e.max(*raw_end))));
                if let Some(e) = last_end {
                    let g = raw_begin.saturating_sub(e);
                    totals.gaps += g;
                    gap = Some(g);
                }
                last_end = Some(last_end.map_or(*raw_end, |e| e.max(*raw_end)));
                pass = passes.iter().enumerate()
                    .filter(|(_, p)| p.raw_begin <= *raw_begin && *raw_begin < p.raw_end.max(p.raw_begin + 1))
                    .max_by_key(|(_, p)| p.depth)
                    .map(|(i, _)| i);
            }
            Timing::Missing => totals.missing += 1,
            _ if op_mismatch || flagged => totals.mismatched += 1,
            _ => totals.untimed += 1,
        }
        if let Some(i) = pass {
            passes[i].draws += 1;
        }
        let r = d.clone();
        let label = (d.label != 0).then(|| c.labels[d.label as usize - 1].clone());
        let vp = (d.vp != 0).then(|| d.vp as usize - 1);
        let fp = (d.fp != 0).then(|| d.fp as usize - 1);
        let item = Draw { raw: r, label, vp, fp, timing, gap, pass, op_mismatch };
        totals.primitives += item.primitives().unwrap_or(0);
        draws.push(item);
    }
    let status = match &c.end {
        None => FrameStatus::Open,
        Some(e) => match e.outcome {
            Outcome::Truncated => FrameStatus::Truncated { at: e.truncated_at.unwrap_or(NO_ORDINAL), reason: e.reason },
            Outcome::Abandoned => FrameStatus::Abandoned { reason: e.reason },
            Outcome::Done if totals.missing > 0 => FrameStatus::PendingTiming { missing: totals.missing },
            Outcome::Done => FrameStatus::Complete,
        },
    };
    FrameView {
        id: b.id,
        name: b.name.clone(),
        source: b.source,
        request_id: b.request_id,
        owner: b.owner,
        frame: b.frame,
        window: b.window,
        isolated: b.isolated,
        status,
        draws,
        programs: c.programs.clone(),
        passes,
        totals,
        end: c.end.clone(),
        draws_not_retained: c.draws_not_retained,
        text_replaced: c.text_replaced,
    }
}

/// What [`group`] groups by.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GroupBy {
    /// (VP, FP) pair.
    Program,
    Vp,
    Fp,
    Label,
    Tag,
    Pass,
    Kind,
}
impl GroupBy {
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s {
            "program" => GroupBy::Program,
            "vp" => GroupBy::Vp,
            "fp" => GroupBy::Fp,
            "label" => GroupBy::Label,
            "tag" => GroupBy::Tag,
            "pass" => GroupBy::Pass,
            "kind" => GroupBy::Kind,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Group {
    pub key: String,
    /// Indices into [`FrameView::draws`], in submission order.
    pub draws: Vec<usize>,
    pub timed: u32,
    pub untimed: u32,
    pub sum: u64,
}

/// A program's display name: its app NAME, else `names` (e.g. the viewer's shader-folder matches by (profile, hash)),
/// else its id text.
pub fn program_name(p: &Program, names: &BTreeMap<(Profile, u64), String>) -> String {
    p.name.clone().or_else(|| names.get(&(p.profile, p.hash)).cloned()).unwrap_or_else(|| p.id_text())
}

/// Groups the draws of `f`, largest timed sum first (ties: first draw first).
pub fn group(f: &FrameView, by: GroupBy, names: &BTreeMap<(Profile, u64), String>) -> Vec<Group> {
    let prog = |i: Option<usize>| i.map_or_else(|| "unknown".to_string(), |i| program_name(&f.programs[i], names));
    let mut map: BTreeMap<String, Group> = BTreeMap::new();
    for (i, d) in f.draws.iter().enumerate() {
        let key = match by {
            GroupBy::Program => format!("{} + {}", prog(d.vp), prog(d.fp)),
            GroupBy::Vp => prog(d.vp),
            GroupBy::Fp => prog(d.fp),
            GroupBy::Label => d.label.clone().unwrap_or_else(|| "(no label)".into()),
            GroupBy::Tag => format!("{:#010x}", d.raw.tag),
            GroupBy::Pass => d.pass.map_or_else(|| "(no pass)".into(), |p| f.passes[p].name.clone()),
            GroupBy::Kind => format!("{} {}", d.raw.kind.name(), primitive_name(d.raw.mode)),
        };
        let g = map.entry(key.clone()).or_insert_with(|| Group { key, draws: Vec::new(), timed: 0, untimed: 0, sum: 0 });
        g.draws.push(i);
        match d.timing.interval() {
            Some(v) => {
                g.timed += 1;
                g.sum += v;
            }
            None => g.untimed += 1,
        }
    }
    let mut v: Vec<Group> = map.into_values().collect();
    v.sort_by(|a, b| b.sum.cmp(&a.sum).then(a.draws[0].cmp(&b.draws[0])));
    v
}

// ---- output ---------------------------------------------------------------------------------------------------------

fn pct(v: u64, total: u64) -> String {
    if total == 0 { "-".into() } else { format!("{:.1}%", v as f64 * 100.0 / total as f64) }
}

/// The CLI text: per capture a header, then the draw list (or one draw, or groups).
pub fn write_text(out: &mut dyn Write, cap: &Capture, frames: &[FrameView], draw: Option<u32>,
                  by: Option<GroupBy>) -> io::Result<()> {
    let Some(inv) = &cap.rsx_frame else {
        return writeln!(out, "rsx-frame: capability absent (file_flags bit 10 clear)");
    };
    if let Some(c) = &inv.config {
        writeln!(out, "rsx-frame: {}; staging {} x {} bytes; programs {} labels {} x {} bytes", c.availability.text(),
            c.blocks, c.block_bytes, c.program_capacity, c.label_capacity, c.label_bytes)?;
    } else {
        writeln!(out, "rsx-frame: no RSXF_CONFIG yet")?;
    }
    if frames.is_empty() {
        return writeln!(out, "no RSX frame captures");
    }
    let names = BTreeMap::new();
    for f in frames {
        writeln!(out, "capture {} \"{}\": owner {} frame {} window {} source {} timing {}; {}", f.id, f.name, f.owner,
            f.frame, f.window, if f.source == Source::Live { format!("live request {}", f.request_id) } else { "app".into() },
            if f.isolated { "ISOLATED" } else { "PIPELINED (reached-marker intervals)" }, f.status.text())?;
        let t = &f.totals;
        writeln!(out, "  draws {} (timed {}, untimed {}, timing absent {}, op mismatch {}); primitives {}; interval sum {} raw; span {}; gaps {} raw{}{}",
            t.draws, t.timed, t.untimed, t.missing, t.mismatched, t.primitives, t.sum,
            t.span.map_or_else(|| "-".into(), |(a, b)| format!("{a}..{b} ({} raw)", b - a)), t.gaps,
            if f.draws_not_retained > 0 { format!("; {} draw records not retained", f.draws_not_retained) } else { String::new() },
            if f.text_replaced > 0 { format!("; {} text fields not UTF-8 (shown with replacements)", f.text_replaced) } else { String::new() })?;
        if let Some(e) = &f.end {
            writeln!(out, "  end: seen {} recorded {} hooked {} decoded-only {} mismatches {}; blocks {} dropped {}; bytes {} dropped {}; programs {} (+{} refused) labels {} (+{} refused); lost events {}",
                e.draws_seen, e.draws_recorded, e.hooked, e.decoded_only, e.mismatches, e.blocks, e.blocks_dropped,
                e.bytes_streamed, e.bytes_dropped, e.programs, e.programs_refused, e.labels, e.labels_refused,
                e.lost_events)?;
            if let Some((h, r)) = e.decoder_reject {
                writeln!(out, "  decoder: first rejection {h:#010x}: {}", auto_rsx::reject_reason(r))?;
            }
        }
        for p in &f.passes {
            writeln!(out, "  pass {} (depth {}): {}..{} ({} raw), {} draws", p.name, p.depth, p.raw_begin, p.raw_end,
                p.raw_end - p.raw_begin, p.draws)?;
        }
        for p in &f.programs {
            writeln!(out, "  program {} {}{}: {} instructions, {} bytes, {} constant blocks, {}, first at {:#x} (draw {}){}{}",
                p.index, p.id_text(), p.name.as_ref().map_or_else(String::new, |n| format!(" \"{n}\"")), p.instructions,
                p.ucode_bytes, p.const_blocks, p.source.name(), p.address, p.first_draw,
                if p.unterminated { ", UNTERMINATED" } else { "" },
                if p.not_revalidated { ", bound before capture, not revalidated" } else { "" })?;
        }
        if let Some(k) = draw {
            match f.draws.iter().find(|d| d.raw.ordinal == k) {
                Some(d) => write_draw(out, f, d, &names)?,
                None => writeln!(out, "  draw {k}: not recorded in this capture")?,
            }
            continue;
        }
        if let Some(by) = by {
            for g in group(f, by, &names) {
                writeln!(out, "  group {}: {} draws (timed {}, untimed {}), {} raw, {}", g.key, g.draws.len(), g.timed,
                    g.untimed, g.sum, pct(g.sum, t.sum))?;
            }
            continue;
        }
        writeln!(out, "  #  kind prim count instances? tag label vp fp pass time gap")?;
        for d in &f.draws {
            writeln!(out, "  {} {} {} {} tag {:#x} label {} vp {} fp {} pass {} time {} gap {}{}", d.raw.ordinal,
                d.raw.kind.name(), primitive_name(d.raw.mode), d.raw.count, d.raw.tag,
                d.label.as_deref().unwrap_or("-"),
                d.vp.map_or_else(|| "-".into(), |i| program_name(&f.programs[i], &names)),
                d.fp.map_or_else(|| "-".into(), |i| program_name(&f.programs[i], &names)),
                d.pass.map_or("-", |p| f.passes[p].name.as_str()), d.timing.text(),
                d.gap.map_or_else(|| "-".into(), |g| g.to_string()),
                if d.op_mismatch || d.raw.flags & DF_MISMATCH != 0 { " MISMATCH" } else { "" })?;
        }
    }
    Ok(())
}

fn write_draw(out: &mut dyn Write, f: &FrameView, d: &Draw, names: &BTreeMap<(Profile, u64), String>) -> io::Result<()> {
    let r = &d.raw;
    writeln!(out, "  draw {}: {} {} first {} count {} primitives {}", r.ordinal, r.kind.name(), primitive_name(r.mode),
        r.first, r.count, d.primitives().map_or_else(|| "-".into(), |p| p.to_string()))?;
    if matches!(r.kind, DrawKind::Index | DrawKind::InlineIndex) {
        writeln!(out, "    index: offset {:#x} type {} location {}", r.index_offset,
            if r.index_type == 1 { "u16" } else { "u32" }, if r.index_location == 1 { "main" } else { "local" })?;
    }
    if r.kind == DrawKind::Clear {
        writeln!(out, "    clear mask {:#x}", r.clear_mask)?;
    }
    writeln!(out, "    time: {}{}", d.timing.text(), d.gap.map_or_else(String::new, |g| format!(", gap {g}")))?;
    writeln!(out, "    tag {:#x} label {}", r.tag, d.label.as_deref().unwrap_or("-"))?;
    writeln!(out, "    vp {} (start {}) fp {} (FP_ADDRESS {:#x})",
        d.vp.map_or_else(|| "unknown".into(), |i| program_name(&f.programs[i], names)), r.vp_start,
        d.fp.map_or_else(|| "unknown".into(), |i| program_name(&f.programs[i], names)), r.fp_address)?;
    writeln!(out, "    seen by: {}{}{}; words {}; caller {:#x}; tb {}", if d.hooked() { "hook " } else { "" },
        if d.decoded() { "stream" } else { "" }, if r.flags & DF_STATE_UNKNOWN != 0 { " (state unknown)" } else { "" },
        r.words, r.caller, r.tb)
}

fn program_json(p: &Program) -> Value {
    json!({"index": p.index, "profile": p.profile.short(), "hash": format!("{:016x}", p.hash), "id": p.id_text(),
        "name": p.name, "source": p.source.name(), "instructions": p.instructions, "ucode_bytes": p.ucode_bytes,
        "const_blocks": p.const_blocks, "address": p.address, "first_draw": p.first_draw,
        "unterminated": p.unterminated, "unreadable": p.unreadable, "not_revalidated": p.not_revalidated})
}

fn timing_json(t: &Timing) -> Value {
    match t {
        Timing::Timed { raw_begin, raw_end } =>
            json!({"state": "timed", "raw_begin": raw_begin, "raw_end": raw_end, "interval": raw_end - raw_begin}),
        Timing::Untimed(r) => json!({"state": "untimed", "reason": untimed_text(*r), "code": r}),
        Timing::Missing => json!({"state": "missing"}),
        Timing::Invalid(why) => json!({"state": "invalid", "reason": why}),
    }
}

fn draw_json(f: &FrameView, d: &Draw) -> Value {
    let r = &d.raw;
    let index = matches!(r.kind, DrawKind::Index | DrawKind::InlineIndex).then(|| {
        json!({"offset": r.index_offset, "type": if r.index_type == 1 { "u16" } else { "u32" },
            "location": if r.index_location == 1 { "main" } else { "local" }})
    });
    let mut v = json!({
        "ordinal": r.ordinal, "kind": r.kind.name(), "primitive": primitive_name(r.mode), "mode": r.mode,
        "first": r.first, "count": r.count, "primitives": d.primitives(), "index": index,
        "clear_mask": r.clear_mask, "tag": r.tag, "label": d.label,
    });
    let o = v.as_object_mut().expect("object");
    o.insert("vp".into(), json!(d.vp.map(|i| f.programs[i].index)));
    o.insert("fp".into(), json!(d.fp.map(|i| f.programs[i].index)));
    o.insert("fp_address".into(), json!(r.fp_address));
    o.insert("vp_start".into(), json!(r.vp_start));
    o.insert("hooked".into(), json!(d.hooked()));
    o.insert("decoded".into(), json!(d.decoded()));
    o.insert("mismatch".into(), json!(d.op_mismatch || r.flags & DF_MISMATCH != 0));
    o.insert("state_unknown".into(), json!(r.flags & DF_STATE_UNKNOWN != 0));
    o.insert("op_id".into(), json!(r.op_id));
    o.insert("timing".into(), timing_json(&d.timing));
    o.insert("gap".into(), json!(d.gap));
    o.insert("pass".into(), json!(d.pass));
    o.insert("caller".into(), json!(r.caller));
    o.insert("words".into(), json!(r.words));
    o.insert("tb".into(), json!(r.tb));
    v
}

fn end_json(e: &End) -> Value {
    let mut v = json!({
        "outcome": format!("{:?}", e.outcome), "reason": end_reason(e.reason), "draws_seen": e.draws_seen,
        "draws_recorded": e.draws_recorded, "hooked": e.hooked, "decoded_only": e.decoded_only,
        "mismatches": e.mismatches, "truncated_at": e.truncated_at, "blocks": e.blocks,
    });
    let o = v.as_object_mut().expect("object");
    o.insert("blocks_dropped".into(), json!(e.blocks_dropped));
    o.insert("bytes_streamed".into(), json!(e.bytes_streamed));
    o.insert("bytes_dropped".into(), json!(e.bytes_dropped));
    o.insert("programs_refused".into(), json!(e.programs_refused));
    o.insert("labels_refused".into(), json!(e.labels_refused));
    o.insert("lost_events".into(), json!(e.lost_events));
    o.insert("decoder_words".into(), json!(e.decoder_words));
    o.insert("untimed".into(), json!(e.untimed));
    o.insert("decoder_reject".into(),
        json!(e.decoder_reject.map(|(h, r)| json!({"header": h, "reason": auto_rsx::reject_reason(r)}))));
    v
}

fn frame_json(f: &FrameView) -> Value {
    let t = &f.totals;
    let totals = json!({"draws": t.draws, "timed": t.timed, "untimed": t.untimed, "missing": t.missing,
        "mismatched": t.mismatched, "sum": t.sum, "gaps": t.gaps, "primitives": t.primitives, "span": t.span.map(|(a, b)| json!([a, b]))});
    let passes: Vec<Value> = f.passes.iter().map(|p| json!({"name": p.name, "depth": p.depth, "tag": p.tag,
        "raw_begin": p.raw_begin, "raw_end": p.raw_end, "draws": p.draws})).collect();
    let mut v = json!({
        "id": f.id, "name": f.name, "source": if f.source == Source::Live { "live" } else { "app" },
        "request_id": f.request_id, "owner": f.owner, "frame": f.frame, "window": f.window,
        "timing_mode": if f.isolated { "isolated" } else { "pipelined" }, "status": f.status.text(),
    });
    let o = v.as_object_mut().expect("object");
    o.insert("totals".into(), totals);
    o.insert("draws_not_retained".into(), json!(f.draws_not_retained));
    o.insert("text_replaced".into(), json!(f.text_replaced));
    o.insert("programs".into(), Value::Array(f.programs.iter().map(program_json).collect()));
    o.insert("passes".into(), Value::Array(passes));
    o.insert("draws".into(), Value::Array(f.draws.iter().map(|d| draw_json(f, d)).collect()));
    o.insert("end".into(), f.end.as_ref().map_or(Value::Null, end_json));
    v
}

/// The JSON document of `ps3dk-prof rsx-frame --json` (schema 1).
pub fn to_json(cap: &Capture, frames: &[FrameView]) -> Value {
    let cfg = cap.rsx_frame.as_ref().and_then(|i| i.config.as_ref());
    json!({
        "schema": 1,
        "capability": cap.rsx_frame.is_some(),
        "availability": cfg.map(|c| c.availability.text()),
        "captures": Value::Array(frames.iter().map(frame_json).collect()),
    })
}

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
use crate::rsx_state::{self, Registers, VpConstants};
use crate::nv40_disasm;
use crate::shader_dir::{Fetches, Folder};
use std::sync::Arc;
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_RSX_FRAME: u32 = 1024;
pub const CHUNK_CONFIG: u32 = 44;
pub const CHUNK_BEGIN: u32 = 45;
pub const CHUNK_DRAWS: u32 = 46;
pub const CHUNK_PROGRAMS: u32 = 47;
pub const CHUNK_END: u32 = 48;
pub const SCHEMA: u16 = 1;
/// CONFIG decoder version: 1 draws and programs only; 2 also the 3D register shadow (BASELINE and STATE records); 3
/// also VP constants (VPCONST), FP embedded constants (FPCONST) and uniform names (UNIFORM).
/// 5: also the DRAW record's ZPASS state (u32@84), its zcull event flags and ZCULL_REGION records.
pub const DECODER: u32 = 5;
pub const REC_UCODE: u16 = 10;
pub const REC_SECOND: u16 = 11;
/// Decoder 5: one zcull region as the application's cellGcmBindZcull / cellGcmUnbindZcull left it (64 bytes).
pub const REC_ZCULL_REGION: u16 = 12;
/// Reserved for zcull statistics reports (report types 2..5): refused until their semantics are characterized.
pub const REC_ZCULL_STATS: u16 = 13;
pub const ZCULL_REGION_BYTES: usize = 64;
pub const ZCULL_REGIONS: u32 = 8;
pub const ZR_BOUND: u32 = 1;
pub const ZR_BEFORE_CAPTURE: u32 = 2;
/// Request / BEGIN flags.
pub const REQ_ISOLATED: u16 = 1;
pub const REQ_FP_BYTES: u16 = 2;
pub const REQ_BOTH: u16 = 4;
/// Decoder 5: ZPASS counting forced on around each captured draw the application had it off for.
pub const REQ_PIXELS: u16 = 8;
pub const CONFIG_BYTES: usize = 64;
pub const BEGIN_FIXED: usize = 56;
pub const DRAWS_HEADER: usize = 16;
pub const END_BYTES: usize = 128;
pub const REC_DRAW: u16 = 1;
pub const REC_PROGRAM: u16 = 2;
pub const REC_LABEL: u16 = 3;
pub const REC_NAME: u16 = 4;
pub const REC_BASELINE: u16 = 5;
pub const REC_STATE: u16 = 6;
/// (register, value) pairs per BASELINE or STATE record, at most.
pub const REG_PAIRS_MAX: usize = 255;
pub const STATE_RESET: u16 = 1;
pub const REC_VPCONST: u16 = 7;
pub const REC_FPCONST: u16 = 8;
pub const REC_UNIFORM: u16 = 9;
pub const VPC_PER_REC: usize = 128;
/// The smallest staging block (32 KiB): the largest per-draw unit (about 31 KiB) fits one, so a unit never spans blocks.
pub const STAGING_MIN: u32 = 32 << 10;
pub const FPC_MAX: usize = 32;
pub const UNI_MAX: usize = 32;
pub const UNI_NAME: usize = 32;
pub const VPC_RESET: u16 = 1;
pub const VPC_BASELINE: u16 = 2;

/// Where one component of an FP embedded constant comes from. None of them is exact: the capture sees the owner's
/// command stream, not memory.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Provenance {
    /// The last inline transfer the decoder saw write this word in the owner's command stream. Not exact: a CPU write
    /// after it, or a write the decoder could not see, is not reflected.
    Inline,
    /// The program container's value: no command-stream write to this word seen since attach, so a CPU write to the
    /// copy in memory would not be seen; it may differ at GPU time.
    Container,
    /// Unknown: the decoder reset or its inline-write map was full since attach, an M2MF copy wrote the word's range,
    /// or a different program was placed at this address after an inline write there (the container's value is shown).
    Unknown,
}
impl Provenance {
    pub fn name(self) -> &'static str {
        match self {
            Provenance::Inline => "last inline write seen",
            Provenance::Container => "container (may differ at GPU time)",
            Provenance::Unknown => "unknown",
        }
    }
}

/// One FP embedded constant at a draw: its ucode byte offset, the provenance of each component and the words as in
/// memory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FpConstant {
    pub offset: u32,
    pub provenance: [Provenance; 4],
    pub words: [u32; 4],
}
impl FpConstant {
    pub fn floats(&self) -> [f32; 4] {
        self.words.map(rsx_state::fp_word_float)
    }
}

/// The FP constants a draw ran with (an FPCONST record; carried to later draws until the next one).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FpConstants {
    /// The FP PROGRAM index (0: unknown FP).
    pub program: u32,
    /// The program has more constant blocks than were captured.
    pub truncated: bool,
    pub entries: Vec<FpConstant>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UniformKind {
    /// A VP constant: `resource` is the first slot, `count` the slots of this table entry.
    VpConstant,
    /// An FP embedded constant: `resource` is the first ucode offset it is patched at, `count` the offsets.
    FpEmbedded,
}

/// One named uniform of a program (UNIFORM record, from the container's parameter table at bind).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Uniform {
    pub name: String,
    pub kind: UniformKind,
    pub resource: u32,
    pub count: u32,
}
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
/// Decoder 5: INVALIDATE_ZCULL / CLEAR_ZCULL_SURFACE written since the previous draw.
pub const DF_ZCULL_INVALIDATED: u16 = 16;
pub const DF_ZCULL_CLEARED: u16 = 32;
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
    /// FP ucode bytes a capture stores at most when they are requested (decoder 4; 0 before).
    pub fp_bytes_cap: u32,
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
    /// Two consecutive frames: the first pipelined, the second isolated (a SECOND record marks the second).
    pub both: bool,
    /// PIXELS: ZPASS counting forced on around draws the application had it off for (decoder 5).
    pub pixels: bool,
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
    /// The AUTO operation kind that times this draw kind. Inline-array and immediate draws are timed as operations of
    /// the ARRAY class and inline-index draws of the INDEX class (the AUTO tables keep three kinds); the capture keeps
    /// the real kind.
    pub fn auto_kind(self) -> Option<u16> {
        match self {
            DrawKind::Array | DrawKind::InlineArray | DrawKind::Immediate => Some(auto_rsx::KIND_ARRAY),
            DrawKind::Index | DrawKind::InlineIndex => Some(auto_rsx::KIND_INDEX),
            DrawKind::Clear => Some(auto_rsx::KIND_CLEAR),
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
    /// Decoder 5: the ZPASS counting state at this draw (u32@84; [`ZpassState`]); 0 before.
    pub zpass: u32,
    pub tb: u64,
    /// The STATE records before this draw: every register unknown first (a decoder reset), then these
    /// (register index, value) pairs.
    pub state_reset: bool,
    pub state: Vec<(u16, u32)>,
    /// The VPCONST records before this draw: every slot unknown first (a reset), then (slot, known mask, words).
    pub vpc_reset: bool,
    pub vpc: Vec<(u16, u8, [u32; 4])>,
    /// The FPCONST record of this draw, if one was written (else the previous draw's list still applies).
    pub fpc: Option<FpConstants>,
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
    /// Its uniforms (UNIFORM record; decoder 3), and whether the parameter table had more than were kept.
    pub uniforms: Vec<Uniform>,
    pub uniforms_truncated: bool,
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
    /// Register pairs in the BASELINE records, and STATE records (decoder 2; both 0 for decoder 1).
    pub baseline_registers: u32,
    pub state_records: u32,
    /// VPCONST, FPCONST and UNIFORM records (decoder 3; 0 before).
    pub vpconst_records: u32,
    pub fpconst_records: u32,
    pub uniform_records: u32,
    /// UCODE records (decoder 4).
    pub ucode_records: u32,
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
    /// The register file at arming (BASELINE records): (register index, value), increasing.
    pub baseline: Vec<(u16, u32)>,
    /// STATE records parsed (also those of draws not retained).
    pub state_records: u32,
    /// The VP constants at arming (VPCONST BASELINE records): (slot, known mask, words), increasing.
    pub vpc_baseline: Vec<(u16, u8, [u32; 4])>,
    pub vpconst_records: u32,
    pub fpconst_records: u32,
    pub uniform_records: u32,
    /// BOTH: the first ordinal and the frame of the second (isolated) frame (SECOND record).
    pub second: Option<(u32, u64)>,
    /// Program bytes (UCODE records), by (profile, hash).
    pub ucode: BTreeMap<(Profile, u64), UcodeParts>,
    pub ucode_records: u32,
    /// ZCULL_REGION records, in order (decoder 5).
    pub zcull_regions: Vec<ZcullRegion>,
}

/// One ZCULL_REGION record: region `index` as bound (or unbound) from draw `ordinal` on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ZcullRegion {
    pub ordinal: u32,
    pub index: u32,
    pub bound: bool,
    /// Bound before the capture was armed (written at arming).
    pub before_capture: bool,
    pub offset: u32,
    pub width: u32,
    pub height: u32,
    pub cull_start: u32,
    pub z_format: u32,
    pub aa_format: u32,
    pub dir: u32,
    pub format: u32,
    pub s_func: u32,
    pub s_ref: u32,
    pub s_mask: u32,
}

/// One program's UCODE records so far: its length, the bytes in order, whether the last part was read.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct UcodeParts {
    pub total: u32,
    pub data: Vec<u8>,
    pub parts: u32,
    pub complete: bool,
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
    /// STATE / VPCONST / FPCONST records read for the next DRAW.
    pending: Option<PendingDraw>,
    /// Programs that already have their UNIFORM record.
    uniformed: Vec<bool>,
    /// A DRAW or STATE record was read in the open capture (a BASELINE must come before both).
    past_baseline: bool,
}

/// The per-draw records read so far for the next DRAW.
#[derive(Debug, Default)]
struct PendingDraw {
    ordinal: u32,
    state_any: bool,
    state_reset: bool,
    state: Vec<(u16, u32)>,
    vpc_any: bool,
    vpc_reset: bool,
    vpc: Vec<(u16, u8, [u32; 4])>,
    fpc: Option<FpConstants>,
}

/// The pending draw for `ordinal` (a new one when none is open): its records must all name the same next draw.
fn pending_for<'a>(p: &'a mut Option<PendingDraw>, ordinal: u32, last: Option<u32>, what: &str)
                   -> Result<&'a mut PendingDraw, Malformed> {
    if last.is_some_and(|o| ordinal <= o) {
        return bad(format!("{what} for draw {ordinal} after draw {}", last.unwrap()));
    }
    if let Some(d) = p.as_ref() {
        if d.ordinal != ordinal {
            return bad(format!("{what} for draw {ordinal} while draw {}'s are open", d.ordinal));
        }
    }
    if p.is_none() {
        *p = Some(PendingDraw { ordinal, ..PendingDraw::default() });
    }
    Ok(p.as_mut().expect("set above"))
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
        let fp_bytes_cap = c.u32()?;
        zeros(&mut c, 28, "RSXF_CONFIG")?;
        if schema != SCHEMA {
            return bad(format!("RSXF_CONFIG schema {schema}"));
        }
        if flags != 0 || !(1..=DECODER).contains(&decoder) {
            return bad(format!("RSXF_CONFIG flags {flags:#x} decoder {decoder}"));
        }
        let Some(availability) = Availability::of(avail) else {
            return bad(format!("RSXF_CONFIG availability {avail}"));
        };
        if !(STAGING_MIN..=1 << 20).contains(&block_bytes) || block_bytes % 8 != 0 || !(1..=16).contains(&blocks)
            || !(1..=65535).contains(&program_capacity) || !(1..=65535).contains(&label_capacity)
            || !(1..=255).contains(&label_bytes)
        {
            return bad(format!(
                "RSXF_CONFIG limits: block {block_bytes} x {blocks}, programs {program_capacity}, labels \
                 {label_capacity} x {label_bytes}"
            ));
        }
        if fp_bytes_cap > 16 << 20 || (decoder < 4 && fp_bytes_cap != 0) {
            return bad(format!("RSXF_CONFIG FP bytes cap {fp_bytes_cap} with decoder {decoder}"));
        }
        self.result.config = Some(Config {
            decoder,
            availability,
            block_bytes,
            blocks,
            program_capacity,
            label_capacity,
            label_bytes,
            fp_bytes_cap,
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
        let req_known = if cfg.decoder >= 5 { 15 } else { 7 };
        if flags & !req_known != 0 || (flags & REQ_ISOLATED != 0 && flags & REQ_BOTH != 0) || known & !3 != 0 || r0 != 0 || r1 != 0 || n > NAME_MAX {
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
                both: flags & REQ_BOTH != 0,
                pixels: flags & REQ_PIXELS != 0,
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
            baseline: Vec::new(),
            state_records: 0,
            vpc_baseline: Vec::new(),
            vpconst_records: 0,
            fpconst_records: 0,
            uniform_records: 0,
            second: None,
            ucode: BTreeMap::new(),
            ucode_records: 0,
            zcull_regions: Vec::new(),
        });
        self.open = true;
        self.last_ordinal = None;
        self.by_hash.clear();
        self.named.clear();
        self.pending = None;
        self.uniformed.clear();
        self.past_baseline = false;
        Ok(())
    }

    fn draws(&mut self, p: &[u8]) -> Result<(), Malformed> {
        if !self.open {
            return bad("RSXF_DRAWS without an open capture");
        }
        let label_bytes = self.result.config.as_ref().expect("checked").label_bytes as usize;
        let decoder = self.result.config.as_ref().expect("checked").decoder;
        let mut c = Cursor::new(p, "RSXF_DRAWS");
        let id = c.u32()?;
        let seq = c.u32()?;
        let n = c.u32()?;
        let bytes = c.u32()? as usize;
        let rec = self.result.captures.last_mut().expect("open");
        if id != rec.begin.id {
            return bad(format!("RSXF_DRAWS for capture {id} while capture {} is open", rec.begin.id));
        }
        let want = rec.block_seqs.last().map_or(0, |&s| s.wrapping_add(1));
        if seq != want {
            return bad(format!("RSXF_DRAWS block sequence {seq}, expected {want} (blocks are written in order, none                 skipped)"));
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
                    self.past_baseline = true;
                    let pd = match self.pending.take() {
                        Some(pd) if pd.ordinal == ordinal => pd,
                        Some(pd) => return bad(format!("records for draw {} before draw {ordinal}", pd.ordinal)),
                        None => PendingDraw::default(),
                    };
                    if let Some(f) = &pd.fpc {
                        match f.program {
                            0 => {}
                            i if rec.programs.get(i as usize - 1).is_some_and(|p| p.profile == Profile::Fragment) => {}
                            i => return bad(format!("draw {ordinal}: FPCONST for program {i}, not a defined FP")),
                        }
                    }
                    let PendingDraw { state_reset, state, vpc_reset, vpc, fpc, .. } = pd;
                    let flags_known = if decoder >= 5 { 63 } else { 15 };
                    if flags & !flags_known != 0 || flags & (DF_HOOKED | DF_DECODED) == 0 || r0 != 0 {
                        return bad(format!("draw {ordinal}: flags {flags:#x}"));
                    }
                    // u32@84: decoder 5 the ZPASS state (5 reserved; FORCED only in a PIXELS capture), zero before
                    if (decoder < 5 && r1 != 0) || r1 > 6 || r1 == 5 || (r1 == 3 && !rec.begin.pixels) {
                        return bad(format!("draw {ordinal}: ZPASS state {r1}"));
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
                        index_location, clear_mask, tag, label, vp, fp, fp_address, vp_start, caller, words,
                        zpass: r1, tb, state_reset, state, vpc_reset, vpc, fpc,
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
                        ucode_bytes, const_blocks, address, first_draw, name: None, uniforms: Vec::new(),
                        uniforms_truncated: false,
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
                REC_BASELINE | REC_STATE => {
                    let ordinal = c.u32()?;
                    let n = c.u16()? as usize;
                    let flags = c.u16()?;
                    let r0 = c.u32()?;
                    if decoder < 2 {
                        return bad(format!("record type {ty} with decoder {decoder}"));
                    }
                    if len != 16 + 8 * n || n > REG_PAIRS_MAX || r0 != 0 {
                        return bad(format!("register record type {ty}: length {len} for {n} pairs"));
                    }
                    let mut pairs = Vec::with_capacity(n);
                    for _ in 0..n {
                        let index = c.u16()?;
                        let r = c.u16()?;
                        let value = c.u32()?;
                        if usize::from(index) >= rsx_state::REGS || r != 0 {
                            return bad(format!("register record type {ty}: register {index}"));
                        }
                        pairs.push((index, value));
                    }
                    if ty == REC_BASELINE {
                        if self.past_baseline || ordinal != 0 || flags != 0 || n == 0 {
                            return bad("BASELINE after a draw, or with an ordinal, flags or no register");
                        }
                        let mut last = rec.baseline.last().map(|p| p.0);
                        for &(i, _) in &pairs {
                            if last.is_some_and(|l| i <= l) {
                                return bad(format!("BASELINE register {i} not increasing"));
                            }
                            last = Some(i);
                        }
                        rec.baseline.extend(pairs);
                    } else {
                        self.past_baseline = true;
                        let reset = flags & STATE_RESET != 0;
                        if flags & !STATE_RESET != 0 || (n == 0 && !reset)
                            || self.last_ordinal.is_some_and(|o| ordinal <= o)
                        {
                            return bad(format!("STATE for draw {ordinal}: flags {flags:#x}, {n} pairs"));
                        }
                        let pd = pending_for(&mut self.pending, ordinal, self.last_ordinal, "STATE")?;
                        if reset && pd.state_any {
                            return bad(format!("STATE for draw {ordinal}: RESET on a later record"));
                        }
                        pd.state_any = true;
                        pd.state_reset |= reset;
                        for &(i, v) in &pairs {
                            if pd.state.last().is_some_and(|l| i <= l.0) {
                                return bad(format!("STATE for draw {ordinal}: register {i} not increasing"));
                            }
                            pd.state.push((i, v));
                        }
                        rec.state_records += 1;
                    }
                }
                REC_VPCONST => {
                    let ordinal = c.u32()?;
                    let n = c.u16()? as usize;
                    let flags = c.u16()?;
                    let r0 = c.u32()?;
                    if decoder < 3 || len != 16 + 24 * n || n > VPC_PER_REC || r0 != 0 {
                        return bad(format!("VPCONST: length {len} for {n} slots, decoder {decoder}"));
                    }
                    let mut list = Vec::with_capacity(n);
                    for _ in 0..n {
                        let slot = c.u16()?;
                        let mask = c.u8()?;
                        let r = c.u8()?;
                        let r2 = c.u32()?;
                        let w = [c.u32()?, c.u32()?, c.u32()?, c.u32()?];
                        if usize::from(slot) >= rsx_state::VP_CONSTS || mask == 0 || mask > 15 || r != 0 || r2 != 0 {
                            return bad(format!("VPCONST: slot {slot} mask {mask:#x}"));
                        }
                        list.push((slot, mask, w));
                    }
                    if flags == VPC_BASELINE {
                        if self.past_baseline || ordinal != 0 || n == 0 {
                            return bad("VPCONST BASELINE after a draw, or with an ordinal or no slot");
                        }
                        let mut last = rec.vpc_baseline.last().map(|p| p.0);
                        for e in &list {
                            if last.is_some_and(|l| e.0 <= l) {
                                return bad(format!("VPCONST BASELINE slot {} not increasing", e.0));
                            }
                            last = Some(e.0);
                        }
                        rec.vpc_baseline.extend(list);
                    } else {
                        self.past_baseline = true;
                        let reset = flags & VPC_RESET != 0;
                        if flags & !VPC_RESET != 0 || (n == 0 && !reset) {
                            return bad(format!("VPCONST for draw {ordinal}: flags {flags:#x}, {n} slots"));
                        }
                        let pd = pending_for(&mut self.pending, ordinal, self.last_ordinal, "VPCONST")?;
                        if reset && pd.vpc_any {
                            return bad(format!("VPCONST for draw {ordinal}: RESET on a later record"));
                        }
                        pd.vpc_any = true;
                        pd.vpc_reset |= reset;
                        for e in list {
                            if pd.vpc.last().is_some_and(|l| e.0 <= l.0) {
                                return bad(format!("VPCONST for draw {ordinal}: slot {} not increasing", e.0));
                            }
                            pd.vpc.push(e);
                        }
                    }
                    rec.vpconst_records += 1;
                }
                REC_FPCONST => {
                    let ordinal = c.u32()?;
                    let n = c.u16()? as usize;
                    let flags = c.u16()?;
                    let program = c.u32()?;
                    if decoder < 3 || len != 16 + 24 * n || n > FPC_MAX || flags & !1 != 0 || (program == 0 && n != 0) {
                        return bad(format!("FPCONST: length {len} for {n} constants, flags {flags:#x}"));
                    }
                    let mut entries: Vec<FpConstant> = Vec::with_capacity(n);
                    for _ in 0..n {
                        let offset = c.u32()?;
                        let mut provenance = [Provenance::Unknown; 4];
                        for p in provenance.iter_mut() {
                            *p = match c.u8()? {
                                1 => Provenance::Inline,
                                2 => Provenance::Container,
                                3 => Provenance::Unknown,
                                v => return bad(format!("FPCONST: provenance {v}")),
                            };
                        }
                        let words = [c.u32()?, c.u32()?, c.u32()?, c.u32()?];
                        if entries.last().is_some_and(|l| offset <= l.offset) {
                            return bad(format!("FPCONST: offset {offset:#x} not increasing"));
                        }
                        entries.push(FpConstant { offset, provenance, words });
                    }
                    self.past_baseline = true;
                    let pd = pending_for(&mut self.pending, ordinal, self.last_ordinal, "FPCONST")?;
                    if pd.fpc.is_some() {
                        return bad(format!("a second FPCONST for draw {ordinal}"));
                    }
                    pd.fpc = Some(FpConstants { program, truncated: flags & 1 != 0, entries });
                    rec.fpconst_records += 1;
                }
                REC_UNIFORM => {
                    let program = c.u32()?;
                    let n = c.u16()? as usize;
                    let flags = c.u16()?;
                    let r0 = c.u32()?;
                    if decoder < 3 || len != 16 + 40 * n || n == 0 || n > UNI_MAX || flags & !1 != 0 || r0 != 0 {
                        return bad(format!("UNIFORM: length {len} for {n} uniforms, flags {flags:#x}"));
                    }
                    let i = program as usize;
                    if i == 0 || i > rec.programs.len() || self.uniformed.get(i - 1).copied().unwrap_or(false) {
                        return bad(format!("UNIFORM for program {program}"));
                    }
                    let profile = rec.programs[i - 1].profile;
                    let mut list = Vec::with_capacity(n);
                    for _ in 0..n {
                        let resource = c.u32()?;
                        let count = u32::from(c.u16()?);
                        let kind = c.u8()?;
                        let name_len = c.u8()? as usize;
                        let raw = c.take(UNI_NAME)?;
                        let kind = match (kind, profile) {
                            (1, Profile::Vertex) => UniformKind::VpConstant,
                            (2, Profile::Fragment) => UniformKind::FpEmbedded,
                            (k, p) => return bad(format!("UNIFORM kind {k} for a {} program", p.short())),
                        };
                        if name_len == 0 || name_len > UNI_NAME || raw[name_len..].iter().any(|&b| b != 0) || count == 0
                            || (kind == UniformKind::VpConstant
                                && resource as usize + count as usize > rsx_state::VP_CONSTS)
                        {
                            return bad(format!("UNIFORM of program {program}: name length {name_len}, resource {resource} x {count}"));
                        }
                        let name = text(&raw[..name_len], &mut rec.text_replaced);
                        list.push(Uniform { name, kind, resource, count });
                    }
                    if self.uniformed.len() < rec.programs.len() {
                        self.uniformed.resize(rec.programs.len(), false);
                    }
                    self.uniformed[i - 1] = true;
                    rec.programs[i - 1].uniforms = list;
                    rec.programs[i - 1].uniforms_truncated = flags & 1 != 0;
                    rec.uniform_records += 1;
                }
                REC_UCODE => {
                    let profile = c.u8()?;
                    let flags = c.u8()?;
                    let part = u32::from(c.u16()?);
                    let total = c.u32()?;
                    let hash = c.u64()?;
                    let offset = c.u32()?;
                    let n = c.u32()? as usize;
                    let r0 = c.u32()?;
                    if decoder < 4 || len != 32 + pad8(n) || n == 0 || n > 2048 || flags & !1 != 0 || r0 != 0
                        || total > 16 << 20 || offset as usize + n > total as usize
                        || (flags & 1 != 0) != (offset as usize + n == total as usize)
                    {
                        return bad(format!("UCODE: part {part} of {n} bytes at {offset} of {total}, flags {flags:#x}"));
                    }
                    let profile = match profile {
                        1 => Profile::Vertex,
                        2 => Profile::Fragment,
                        v => return bad(format!("UCODE profile {v}")),
                    };
                    let data = c.take(n)?.to_vec();
                    zeros(&mut c, pad8(n) - n, "UCODE")?;
                    let u = rec.ucode.entry((profile, hash)).or_default();
                    if u.complete || part != u.parts || offset as usize != u.data.len() || (part > 0 && total != u.total) {
                        return bad(format!("UCODE {}:{hash:016x}: part {part} at {offset} out of sequence", profile.short()));
                    }
                    u.total = total;
                    u.parts += 1;
                    u.data.extend_from_slice(&data);
                    u.complete = flags & 1 != 0;
                    rec.ucode_records += 1;
                }
                REC_SECOND => {
                    let first = c.u32()?;
                    let frame = c.u64()?;
                    if decoder < 4 || len != 16 || !rec.begin.both || rec.second.is_some() || self.pending.is_some()
                        || self.last_ordinal.is_some_and(|o| first <= o)
                    {
                        return bad(format!("SECOND at draw {first}"));
                    }
                    rec.second = Some((first, frame));
                }
                REC_ZCULL_REGION => {
                    let ordinal = c.u32()?;
                    let v: Vec<u32> = (0..13).map(|_| c.u32()).collect::<Result<_, _>>()?;
                    let r = c.u32()?;
                    let (index, flags) = (v[0], v[1]);
                    let bound = flags & ZR_BOUND != 0;
                    if decoder < 5 || len != ZCULL_REGION_BYTES || index >= ZCULL_REGIONS
                        || flags & !(ZR_BOUND | ZR_BEFORE_CAPTURE) != 0 || r != 0
                        || (!bound && v[2..].iter().any(|&x| x != 0))
                        || (flags & ZR_BEFORE_CAPTURE != 0 && (ordinal != 0 || self.past_baseline))
                        || self.last_ordinal.is_some_and(|o| ordinal <= o) || self.pending.is_some()
                    {
                        return bad(format!("ZCULL_REGION {index} at draw {ordinal}, flags {flags:#x}"));
                    }
                    rec.zcull_regions.push(ZcullRegion { ordinal, index, bound,
                        before_capture: flags & ZR_BEFORE_CAPTURE != 0, offset: v[2], width: v[3], height: v[4],
                        cull_start: v[5], z_format: v[6], aa_format: v[7], dir: v[8], format: v[9], s_func: v[10],
                        s_ref: v[11], s_mask: v[12] });
                }
                REC_ZCULL_STATS => {
                    return bad("record type 13 (zcull statistics) is reserved: not defined yet");
                }
                _ => return bad(format!("record type {ty}")),
            }
        }
        if c.remaining() != 0 {
            return bad("RSXF_DRAWS: bytes after the last record");
        }
        if self.pending.is_some() {
            return bad("RSXF_DRAWS: a STATE, VPCONST or FPCONST record without its DRAW in the same block");
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
        let baseline_registers = c.u32()?;
        let vpconst_records = c.u32()?;
        let fpconst_records = c.u32()?;
        let untimed = c.u32()?;
        let state_records = c.u32()?;
        let uniform_records = c.u32()?;
        let ucode_records = c.u32()?;
        let (r2, r3) = (0u32, 0u32);
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
        if reason > 7 || r2 != 0 || r3 != 0 || (rej_reason == 0 && rej_header != 0) {
            return bad(format!("RSXF_END reason {reason} or reserved fields"));
        }
        let recorded = rec.draws.len() as u64 + rec.draws_not_retained;
        if blocks as usize != rec.block_seqs.len() {
            return bad(format!("RSXF_END: {blocks} blocks emitted, {} seen", rec.block_seqs.len()));
        }
        if blocks_dropped != 0 {
            return bad(format!("RSXF_END: {blocks_dropped} blocks dropped (reserved 0: blocks are written in order,                 none skipped)"));
        }
        if u64::from(draws_recorded) != recorded || draws_recorded > draws_seen
            || hooked.saturating_add(decoded_only) > draws_seen || untimed > draws_seen
        {
            return bad(format!("RSXF_END draw counts: seen {draws_seen} recorded {draws_recorded} (records {recorded})"));
        }
        if vpconst_records != rec.vpconst_records || fpconst_records != rec.fpconst_records
            || uniform_records != rec.uniform_records || ucode_records != rec.ucode_records
        {
            return bad(format!("RSXF_END: {vpconst_records} VPCONST, {fpconst_records} FPCONST and {uniform_records} \
                UNIFORM records against {}, {} and {}", rec.vpconst_records, rec.fpconst_records, rec.uniform_records));
        }
        if baseline_registers as usize != rec.baseline.len() || state_records != rec.state_records {
            return bad(format!("RSXF_END: {baseline_registers} baseline registers and {state_records} STATE records \
                against {} and {}", rec.baseline.len(), rec.state_records));
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
            baseline_registers, state_records, vpconst_records, fpconst_records, uniform_records, ucode_records,
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
    container_ucode(bytes).map(|(h, _)| h)
}

/// [`container_hash`], with the ucode bytes the hash walked from (the container's whole ucode).
pub fn container_ucode(bytes: &[u8]) -> Result<(ProgramHash, &[u8]), String> {
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
        CG_PROFILE_VP => Ok((vp_hash(ucode), ucode)),
        CG_PROFILE_FP => Ok((fp_hash(ucode), ucode)),
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
    /// The 3D register file at this draw (decoder 2 captures; None for decoder 1), shared with the draws after it
    /// until a register changes. Interpret it with [`rsx_state::interpret`].
    pub registers: Option<Arc<Registers>>,
    /// The registers (byte addresses) this draw's STATE records changed, increasing; every known one after a reset.
    pub changed: Vec<u32>,
    /// The VP constant file at this draw (decoder 3), and the slots its VPCONST records changed.
    pub vp_constants: Option<Arc<VpConstants>>,
    pub vpc_changed: Vec<u16>,
    /// The FP embedded constants it ran with (decoder 3; the latest FPCONST up to this draw).
    pub fp_constants: Option<Arc<FpConstants>>,
    /// Index into [`FrameView::instance_runs`].
    pub run: Option<usize>,
    /// The ZPASS counter's end - begin over this draw (its timed AUTO operation's OP_ZPASS value); a sample count only
    /// when [`Draw::zpass_state`] says counting was on. See [`pixels`].
    pub zpass_delta: Option<u32>,
}

/// Consecutive draws identical except for their constants (same kind, primitive, counts, index stream, programs,
/// label, tag and register state): an instance run, drawn once per set of constants.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstanceRun {
    /// Indices into [`FrameView::draws`]: the first draw and how many.
    pub first: usize,
    pub len: usize,
    /// VP constant slots and FP constant offsets whose values differ within the run.
    pub vp_slots: Vec<u16>,
    pub fp_offsets: Vec<u32>,
}

/// One uniform's value at a draw.
#[derive(Debug, Clone, PartialEq)]
pub struct UniformValue {
    pub profile: Profile,
    pub name: String,
    pub resource: u32,
    /// One vec4 per slot (VP) or per embedded constant (FP); a component None when unknown (FP: provenance UNKNOWN).
    /// FP values are never exact: see `provenance`.
    pub values: Vec<[Option<f32>; 4]>,
    /// FP: where each component comes from (None for VP: the stream is exact).
    pub provenance: Option<[Provenance; 4]>,
}
impl Draw {
    pub fn hooked(&self) -> bool {
        self.raw.flags & DF_HOOKED != 0
    }
    pub fn decoded(&self) -> bool {
        self.raw.flags & DF_DECODED != 0
    }
    /// Primitives drawn (None for a clear, an inline array, whose count is data words, and an immediate draw).
    pub fn primitives(&self) -> Option<u64> {
        match self.raw.kind {
            DrawKind::Clear | DrawKind::InlineArray | DrawKind::Immediate => None,
            _ => primitive_count(self.raw.mode, self.raw.count),
        }
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
    /// The register file at arming (decoder 2 captures).
    pub baseline: Option<Arc<Registers>>,
    pub instance_runs: Vec<InstanceRun>,
    /// Which frame of the capture this view is: the only one, or the pipelined first / isolated second of a BOTH
    /// capture (each view has one timing mode, [`FrameView::isolated`]).
    pub half: Half,
    /// On the second view of a BOTH capture: its draws paired with the first view's ([`pair_draws`]).
    pub pairs: Vec<DrawPair>,
    /// Program ucode bytes stored in the capture (UCODE records), by (profile, hash); [`FrameView::bytes_of`].
    pub program_bytes: BTreeMap<(Profile, u64), Arc<ProgramBytes>>,
    /// The capture's ZCULL_REGION records, in order (decoder 5).
    pub zcull_regions: Vec<ZcullRegion>,
    /// The CONFIG decoder version (5: ZPASS states, zcull events and regions are recorded).
    pub decoder: u32,
    /// PIXELS: counting was forced on around draws the application had it off for.
    pub pixels_forced: bool,
}

/// Which frame of its capture a [`FrameView`] shows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Half {
    Only,
    /// BOTH: the first frame, pipelined.
    First,
    /// BOTH: the second frame, isolated.
    Second,
}

/// A draw of a BOTH capture's first frame and its counterpart in the second (indices into each view's draws; None:
/// unpaired).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrawPair {
    pub first: Option<usize>,
    pub second: Option<usize>,
}

/// One program's ucode bytes (a disassembler's input). VP: the instructions as the command stream uploaded them, 16
/// bytes each, 32-bit words big-endian, from the start slot through the last instruction. FP: the bytes as they lay in
/// memory at the bind (each 32-bit word big-endian with its 16-bit halves swapped, constant blocks inline after the
/// instruction that reads them), through the end instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProgramBytes {
    pub profile: Profile,
    pub hash: u64,
    /// The program's length in bytes, and the bytes captured (all of them when `complete`).
    pub total: u32,
    pub bytes: Vec<u8>,
    pub complete: bool,
}

impl FrameView {
    /// The stored bytes of program `index` (into [`FrameView::programs`]), if any.
    pub fn bytes_of(&self, index: usize) -> Option<&ProgramBytes> {
        let p = self.programs.get(index)?;
        (!p.unreadable).then(|| self.program_bytes.get(&(p.profile, p.hash)).map(|b| b.as_ref())).flatten()
    }
}

/// The pairing key of a draw: kind, label, and the (profile, hash) of its programs.
fn pair_key(f: &FrameView, d: &Draw) -> (DrawKind, Option<String>, Option<(Profile, u64)>, Option<(Profile, u64)>) {
    let id = |i: Option<usize>| i.map(|i| (f.programs[i].profile, f.programs[i].hash));
    (d.raw.kind, d.label.clone(), id(d.vp), id(d.fp))
}

/// Pairs the draws of a BOTH capture's two frames: in submission order, a draw pairs with the next draw of the other
/// frame with the same kind, label and program pair (looking up to 16 draws ahead); the rest are unpaired.
pub fn pair_draws(a: &FrameView, b: &FrameView) -> Vec<DrawPair> {
    let (ka, kb): (Vec<_>, Vec<_>) = (a.draws.iter().map(|d| pair_key(a, d)).collect(),
        b.draws.iter().map(|d| pair_key(b, d)).collect());
    let (mut i, mut j, mut out) = (0, 0, Vec::new());
    while i < ka.len() || j < kb.len() {
        if i < ka.len() && j < kb.len() && ka[i] == kb[j] {
            out.push(DrawPair { first: Some(i), second: Some(j) });
            i += 1;
            j += 1;
        } else {
            let ahead = if i < ka.len() { (j..kb.len().min(j + 16)).find(|&k| kb[k] == ka[i]) } else { None };
            if let Some(k) = ahead {
                for s in j..k {
                    out.push(DrawPair { first: None, second: Some(s) });
                }
                j = k;
            } else if i < ka.len() {
                out.push(DrawPair { first: Some(i), second: None });
                i += 1;
            } else {
                out.push(DrawPair { first: None, second: Some(j) });
                j += 1;
            }
        }
    }
    out
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
    inv.captures.iter().flat_map(|c| {
        if !c.begin.both {
            return vec![view(cap, c, &ops, Half::Only)];
        }
        let first = view(cap, c, &ops, Half::First);
        if c.second.is_none() {
            return vec![first];
        }
        let mut second = view(cap, c, &ops, Half::Second);
        second.pairs = pair_draws(&first, &second);
        vec![first, second]
    }).collect()
}

fn view(cap: &Capture, c: &CaptureRecord, ops: &HashMap<u64, &Op>, half: Half) -> FrameView {
    let b = &c.begin;
    // the frame this view shows and its draw ordinals [lo, hi); a view never mixes timing modes
    let (frame, lo, hi, isolated) = match (half, c.second) {
        (Half::First, Some((o, _))) => (b.frame, 0u64, u64::from(o), false),
        (Half::Second, Some((o, f))) => (f, u64::from(o), u64::MAX, true),
        (Half::First, None) => (b.frame, 0, u64::MAX, false),
        _ => (b.frame, 0, u64::MAX, b.isolated),
    };
    // passes: the frame's AUTO ranges
    let mut passes: Vec<Pass> = cap.auto.as_ref().map_or_else(Vec::new, |a| {
        a.ops.iter()
            .filter(|o| o.kind == auto_rsx::KIND_RANGE && o.token() == Some((b.owner, frame)))
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
    // the register file, replayed from the baseline through each draw's STATE records
    let stateful = cap.rsx_frame.as_ref().and_then(|i| i.config.as_ref()).is_some_and(|k| k.decoder >= 2);
    let baseline = stateful.then(|| {
        let mut r = Registers::new();
        for &(i, v) in &c.baseline {
            r.set(i, v);
        }
        Arc::new(r)
    });
    let mut regs = baseline.clone();
    let constants = cap.rsx_frame.as_ref().and_then(|i| i.config.as_ref()).is_some_and(|k| k.decoder >= 3);
    let mut vpcs = constants.then(|| {
        let mut v = VpConstants::new();
        for &(s, m, w) in &c.vpc_baseline {
            v.set(s, m, w);
        }
        Arc::new(v)
    });
    let mut fpcs: Option<Arc<FpConstants>> = None;
    let mut totals = Totals::default();
    let mut last_end: Option<u64> = None;
    for d in &c.draws {
        let mut changed = Vec::new();
        if let Some(cur) = regs.as_mut() {
            if d.state_reset || !d.state.is_empty() {
                let next = Arc::make_mut(cur);
                if d.state_reset {
                    next.clear();
                }
                for &(i, v) in &d.state {
                    next.set(i, v);
                    changed.push(u32::from(i) * 4);
                }
            }
        }
        let mut vpc_changed = Vec::new();
        if let Some(cur) = vpcs.as_mut() {
            if d.vpc_reset || !d.vpc.is_empty() {
                let next = Arc::make_mut(cur);
                if d.vpc_reset {
                    next.clear();
                }
                for &(s, m, w) in &d.vpc {
                    next.set(s, m, w);
                    vpc_changed.push(s);
                }
            }
        }
        if let Some(f) = &d.fpc {
            fpcs = Some(Arc::new(f.clone()));
        }
        if !(lo..hi).contains(&u64::from(d.ordinal)) {
            continue; // the other frame of a BOTH capture (its state is still replayed above)
        }
        let mut op_mismatch = false;
        let mut zpass_delta = None;
        let flagged = d.flags & DF_MISMATCH != 0;
        let timing = if d.op_id == 0 {
            Timing::Untimed(d.untimed)
        } else {
            match ops.get(&d.op_id) {
                None => Timing::Missing,
                Some(o) => {
                    op_mismatch = d.kind.auto_kind() != Some(o.kind) || o.token() != Some((b.owner, frame));
                    if op_mismatch {
                        // the op is not this draw's (another kind or frame): its interval is not the draw's time
                        Timing::Invalid("AUTO operation of another kind or frame")
                    } else if flagged {
                        // the runtime saw the hooked call and the decoded words disagree: not this draw's time either
                        Timing::Invalid("hooked call and decoded draw disagree")
                    } else {
                        match op_interval(o) {
                            Ok((rb, re)) => {
                                zpass_delta = o.zpass;
                                Timing::Timed { raw_begin: rb, raw_end: re }
                            }
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
        let item = Draw { raw: r, label, vp, fp, timing, gap, pass, op_mismatch, registers: regs.clone(), changed,
            vp_constants: vpcs.clone(), vpc_changed, fp_constants: if constants { fpcs.clone() } else { None },
            run: None, zpass_delta };
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
    let mut view = FrameView {
        id: b.id,
        name: b.name.clone(),
        source: b.source,
        request_id: b.request_id,
        owner: b.owner,
        frame,
        window: b.window,
        isolated,
        status,
        draws,
        programs: c.programs.clone(),
        passes,
        totals,
        end: c.end.clone(),
        draws_not_retained: c.draws_not_retained,
        text_replaced: c.text_replaced,
        baseline,
        instance_runs: Vec::new(),
        half,
        pairs: Vec::new(),
        program_bytes: c.ucode.iter().filter(|(_, u)| !u.data.is_empty()).map(|(&(profile, hash), u)| {
            ((profile, hash), Arc::new(ProgramBytes { profile, hash, total: u.total, bytes: u.data.clone(),
                complete: u.complete }))
        }).collect(),
        zcull_regions: c.zcull_regions.clone(),
        decoder: cap.rsx_frame.as_ref().and_then(|i| i.config.as_ref()).map_or(0, |k| k.decoder),
        pixels_forced: b.pixels,
    };
    let runs = instance_runs(&view);
    for (k, r) in runs.iter().enumerate() {
        for d in &mut view.draws[r.first..r.first + r.len] {
            d.run = Some(k);
        }
    }
    view.instance_runs = runs;
    view
}

/// The two draws differ only in their constants.
fn same_but_constants(a: &Draw, b: &Draw) -> bool {
    let (x, y) = (&a.raw, &b.raw);
    (x.kind, x.mode, x.first, x.count, x.index_offset, x.index_type, x.index_location, x.clear_mask, x.tag)
        == (y.kind, y.mode, y.first, y.count, y.index_offset, y.index_type, y.index_location, y.clear_mask, y.tag)
        && (a.vp, a.fp, &a.label) == (b.vp, b.fp, &b.label)
        && a.vp.is_some()
        && match (&a.registers, &b.registers) {
            (Some(p), Some(q)) => Arc::ptr_eq(p, q) || p == q,
            _ => false,
        }
}

/// Every instance run of `f` (two or more draws), in draw order.
pub fn instance_runs(f: &FrameView) -> Vec<InstanceRun> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < f.draws.len() {
        let mut j = i + 1;
        while j < f.draws.len() && same_but_constants(&f.draws[j - 1], &f.draws[j]) {
            j += 1;
        }
        if j - i >= 2 {
            let mut vp_slots: Vec<u16> = f.draws[i + 1..j].iter().flat_map(|d| d.vpc_changed.iter().copied()).collect();
            vp_slots.sort_unstable();
            vp_slots.dedup();
            let mut fp_offsets = Vec::new();
            for w in f.draws[i..j].windows(2) {
                if let (Some(a), Some(b)) = (&w[0].fp_constants, &w[1].fp_constants) {
                    for e in &b.entries {
                        if a.entries.iter().find(|x| x.offset == e.offset).map(|x| x.words) != Some(e.words) {
                            fp_offsets.push(e.offset);
                        }
                    }
                }
            }
            fp_offsets.sort_unstable();
            fp_offsets.dedup();
            out.push(InstanceRun { first: i, len: j - i, vp_slots, fp_offsets });
        }
        i = j;
    }
    out
}

/// The values of the uniforms of a draw's programs (decoder 3 captures; empty otherwise).
pub fn uniforms(f: &FrameView, d: &Draw) -> Vec<UniformValue> {
    let mut out = Vec::new();
    if let (Some(i), Some(v)) = (d.vp, &d.vp_constants) {
        for u in &f.programs[i].uniforms {
            let values = (u.resource..u.resource + u.count).map(|s| v.floats(s as usize)).collect();
            out.push(UniformValue { profile: Profile::Vertex, name: u.name.clone(), resource: u.resource, values,
                provenance: None });
        }
    }
    if let (Some(i), Some(fc)) = (d.fp, &d.fp_constants) {
        if fc.program as usize == i + 1 {
            for u in &f.programs[i].uniforms {
                let e = fc.entries.iter().find(|e| e.offset == u.resource);
                out.push(UniformValue { profile: Profile::Fragment, name: u.name.clone(), resource: u.resource,
                    // an UNKNOWN component is None: the word the record carries for it is only the container's
                    // fallback, not the value the draw ran with
                    values: vec![e.map_or([None; 4], |e| {
                        let f = e.floats();
                        std::array::from_fn(|k| (e.provenance[k] != Provenance::Unknown).then_some(f[k]))
                    })],
                    provenance: Some(e.map_or([Provenance::Unknown; 4], |e| e.provenance)) });
            }
        }
    }
    out
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

/// Where a program's static texture fetch counts come from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FetchOrigin {
    /// The program bytes the capture kept (complete).
    Capture,
    /// A container in the shader folder with the program's canonical hash.
    ShaderFolder,
}
impl FetchOrigin {
    pub fn name(self) -> &'static str {
        match self {
            FetchOrigin::Capture => "capture bytes",
            FetchOrigin::ShaderFolder => "shader folder",
        }
    }
}

/// Program `index`'s static texture fetches (see [`nv40_disasm::TexFetches`]): from the capture's complete bytes,
/// else from `folder` (a shader-folder scan) by its canonical hash; None when neither has the program (or its
/// identity is unavailable).
pub fn program_fetches(f: &FrameView, index: usize, folder: &crate::shader_dir::Fetches)
                       -> Option<(nv40_disasm::TexFetches, FetchOrigin)> {
    let p = f.programs.get(index)?;
    if p.unreadable {
        return None;
    }
    if let Some(b) = f.bytes_of(index).filter(|b| b.complete) {
        return Some((crate::shader_dir::tex_fetches(b.profile, &b.bytes), FetchOrigin::Capture));
    }
    folder.get(&(p.profile, p.hash)).map(|t| (*t, FetchOrigin::ShaderFolder))
}

/// Whether a fragment texture slot is bound for a draw.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Bound {
    /// Enabled at the draw and sampled by its fragment program.
    Yes,
    /// Disabled at the draw: whatever offset or format the slot still holds is stale state from an earlier bind.
    Disabled,
    /// Enabled, but the draw's fragment program has no fetch from this unit (exact counts).
    NotSampled,
    /// The enable state is unknown (not written since the last decoder reset).
    EnableUnknown,
    /// Enabled; whether the fragment program samples it is unknown (no complete program bytes, or counts that are
    /// only a lower bound).
    SamplingUnknown,
}
impl Bound {
    /// true / false / unknown.
    pub fn flag(self) -> Option<bool> {
        match self {
            Bound::Yes => Some(true),
            Bound::Disabled | Bound::NotSampled => Some(false),
            Bound::EnableUnknown | Bound::SamplingUnknown => None,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Bound::Yes => "bound",
            Bound::Disabled => "disabled",
            Bound::NotSampled => "enabled, not sampled",
            Bound::EnableUnknown => "enable unknown",
            Bound::SamplingUnknown => "enabled, sampling unknown",
        }
    }
    pub fn reason(self) -> &'static str {
        match self {
            Bound::Yes => "enabled at this draw and sampled by its fragment program",
            Bound::Disabled => "disabled at this draw; the offset and format it still holds are stale state",
            Bound::NotSampled => "enabled at this draw, but its fragment program has no fetch from this unit",
            Bound::EnableUnknown => "the enable state is not known (not written since the last decoder reset)",
            Bound::SamplingUnknown =>
                "enabled at this draw; whether its fragment program samples it is not known (no program bytes)",
        }
    }
}

/// One fragment texture slot at a draw.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TextureSlot {
    pub slot: u32,
    /// The slot's enable bit (TEX_CONTROL0 bit 31); None when unknown.
    pub enabled: Option<bool>,
    /// The fragment program's fetch instructions from this unit; None when not known (no program bytes, or none
    /// counted in a program whose counts are only a lower bound).
    pub fetches: Option<u32>,
    pub bound: Bound,
}

/// The 16 fragment texture slots of draw `d`, given its fragment program's fetches (from [`program_fetches`]); None
/// for a draw without register state (decoder 1).
pub fn texture_slots(d: &Draw, fp: Option<&nv40_disasm::TexFetches>) -> Option<Vec<TextureSlot>> {
    let regs = d.registers.as_ref()?;
    let view = rsx_state::interpret(regs);
    Some(view.textures.iter().enumerate().map(|(i, t)| {
        let enabled = match t.iter().find(|f| f.name == "enable").map(|f| &f.value) {
            Some(rsx_state::Value::Bool(b)) => Some(*b),
            _ => None,
        };
        let fetches = fp.and_then(|t| {
            let n = t.by_unit[i];
            (n > 0 || t.exact()).then_some(n)
        });
        let bound = match (enabled, fetches) {
            (Some(false), _) => Bound::Disabled,
            (None, _) => Bound::EnableUnknown,
            (Some(true), Some(n)) if n > 0 => Bound::Yes,
            (Some(true), Some(_)) => Bound::NotSampled,
            (Some(true), None) => Bound::SamplingUnknown,
        };
        TextureSlot { slot: i as u32, enabled, fetches, bound }
    }).collect())
}

/// A draw's ZPASS pixel counting state (DRAW u32@84, decoder 5).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZpassState {
    /// Not recorded: the capture's decoder is older than 5.
    NotRecorded,
    /// The decoder reset since attach: unseen words may have changed it.
    Unknown,
    /// The application had counting on.
    Counting,
    /// The application had counting off.
    Off,
    /// A PIXELS capture turned counting on around this draw.
    Forced,
    /// Off, but the application used the counter in this frame (or carried its use in): never forced.
    Paused,
    /// No ZPASS method was seen since the decoder attached: counting assumed off (PIXELS forces it).
    Untouched,
}
impl ZpassState {
    pub fn of(f: &FrameView, d: &Draw) -> Self {
        if f.decoder < 5 {
            return ZpassState::NotRecorded;
        }
        match d.raw.zpass {
            1 => ZpassState::Counting,
            2 => ZpassState::Off,
            3 => ZpassState::Forced,
            4 => ZpassState::Paused,
            6 => ZpassState::Untouched,
            _ => ZpassState::Unknown,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            ZpassState::NotRecorded => "not recorded",
            ZpassState::Unknown => "unknown",
            ZpassState::Counting => "counting",
            ZpassState::Off => "off",
            ZpassState::Forced => "forced",
            ZpassState::Paused => "paused",
            ZpassState::Untouched => "untouched",
        }
    }
    /// Why a draw in this state has no sample count (None when it has one, or would with a timed operation).
    pub fn why_uncounted(self) -> Option<&'static str> {
        match self {
            ZpassState::Counting | ZpassState::Forced => None,
            ZpassState::NotRecorded => Some("the capture's decoder records no ZPASS state (older than 5)"),
            ZpassState::Unknown => Some("counting state unknown (the decoder reset since it attached)"),
            ZpassState::Off => Some("the application had ZPASS counting off (request the capture with PIXELS to force it)"),
            ZpassState::Paused => Some("counting off, but the application used the ZPASS counter in this frame (its occlusion query): the capture never forces it then"),
            ZpassState::Untouched => Some("counting never enabled (request the capture with PIXELS to force it)"),
        }
    }
}

/// Whether depth rejection before shading can apply to a draw, from its fragment program.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EarlyZ {
    /// The program kills fragments (FP_CONTROL KIL, or a KIL in its ucode): every rasterized sample is shaded, so
    /// shading work is at least the passed-sample count.
    Disabled,
    /// No KIL: early rejection may apply, so shading work is about the passed-sample count (unless the program
    /// writes depth, which the capture does not detect).
    Eligible,
    Unknown,
}
impl EarlyZ {
    pub fn name(self) -> &'static str {
        match self {
            EarlyZ::Disabled => "off (the program kills fragments)",
            EarlyZ::Eligible => "eligible",
            EarlyZ::Unknown => "unknown",
        }
    }
    /// How shading work relates to the passed-sample count.
    pub fn shading(self) -> &'static str {
        match self {
            EarlyZ::Disabled => "shading >= count",
            EarlyZ::Eligible => "shading ~ count (unless the program writes depth)",
            EarlyZ::Unknown => "shading vs count unknown",
        }
    }
}

/// One draw's passed samples and what they mean (see docs/profiler-guide.md, "Pixels per draw").
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pixels {
    pub state: ZpassState,
    /// Samples that passed every per-fragment test (depth, stencil, alpha, KIL; early-z and zcull rejected ones are
    /// not counted), when counting was on and the draw was timed.
    pub samples: Option<u32>,
    /// The draw's render target area in samples (clip width x height x samples per pixel), when known.
    pub area: Option<u64>,
    /// samples / area: this draw's share of a full-target pass (its overdraw contribution).
    pub per_pixel: Option<f64>,
    /// ISOLATED views only: the draw's interval (raw RSX ticks) per passed sample.
    pub raw_per_sample: Option<f64>,
    pub early_z: EarlyZ,
    /// samples x the FP's instruction count, and x its texture fetches (static): shading and fetch work proxies.
    pub shading_proxy: Option<u64>,
    pub fetch_proxy: Option<u64>,
}

/// Samples per pixel of the draw's surface (the antialias mode), when known.
fn aa_samples(v: &rsx_state::StateView) -> Option<u64> {
    match v.surfaces.iter().find(|f| f.name == "antialias").map(|f| &f.value) {
        Some(rsx_state::Value::Name(n)) if n.ends_with("_1") => Some(1),
        Some(rsx_state::Value::Name(n)) if n.ends_with("_2") => Some(2),
        Some(rsx_state::Value::Name(n)) if n.ends_with("_4") => Some(4),
        _ => None,
    }
}
fn uint_field(fs: &[rsx_state::Field], name: &str) -> Option<u64> {
    match fs.iter().find(|f| f.name == name).map(|f| &f.value) {
        Some(rsx_state::Value::Uint(v)) => Some(u64::from(*v)),
        Some(rsx_state::Value::Hex(v)) => Some(u64::from(*v)),
        _ => None,
    }
}

/// The passed samples of draw `d` and their derived metrics; `folder` supplies program bytes the capture did not keep
/// (for the KIL check and the proxies).
pub fn pixels(f: &FrameView, d: &Draw, folder: &Fetches) -> Pixels {
    let state = ZpassState::of(f, d);
    let samples = matches!(state, ZpassState::Counting | ZpassState::Forced).then_some(d.zpass_delta).flatten();
    let view = d.registers.as_ref().map(|r| rsx_state::interpret(r));
    let area = view.as_ref().and_then(|v| {
        Some(uint_field(&v.surfaces, "clip.width")? * uint_field(&v.surfaces, "clip.height")? * aa_samples(v)?)
    }).filter(|&a| a > 0);
    let fp = d.fp.and_then(|i| program_fetches(f, i, folder));
    let kil_reg = d.registers.as_ref().and_then(|r| r.get(0x1d60)).map(|c| c & 0x80 != 0);
    let early_z = if d.raw.kind == DrawKind::Clear {
        EarlyZ::Unknown
    } else if kil_reg == Some(true) || fp.is_some_and(|(t, _)| t.kil > 0) {
        EarlyZ::Disabled
    } else if kil_reg == Some(false) || fp.is_some_and(|(t, _)| t.exact()) {
        EarlyZ::Eligible
    } else {
        EarlyZ::Unknown
    };
    let insns = d.fp.and_then(|i| f.programs.get(i)).filter(|p| !p.unreadable).map(|p| u64::from(p.instructions));
    Pixels {
        state,
        samples,
        area,
        per_pixel: samples.zip(area).map(|(s, a)| s as f64 / a as f64),
        raw_per_sample: samples.filter(|&s| s > 0 && f.isolated)
            .and_then(|s| d.timing.interval().map(|t| t as f64 / s as f64)),
        early_z,
        shading_proxy: samples.zip(insns).map(|(s, n)| u64::from(s) * n),
        fetch_proxy: samples.zip(fp).map(|(s, (t, _))| u64::from(s) * u64::from(t.total())),
    }
}

/// Passed samples per render target over a view: the overdraw of each target.
#[derive(Debug, Clone, PartialEq)]
pub struct TargetOverdraw {
    /// `color0 <offset> / zeta <offset> <w>x<h> x<aa>`, or "unknown" without register state.
    pub key: String,
    pub draws: u32,
    /// Draws with a sample count, and their samples.
    pub counted: u32,
    pub samples: u64,
    /// The target's area in samples; samples / area is the overdraw (1.0: every sample written once).
    pub area: Option<u64>,
    pub overdraw: Option<f64>,
}

/// Overdraw per render target (draws that are not clears), in first-draw order.
pub fn overdraw(f: &FrameView) -> Vec<TargetOverdraw> {
    let mut out: Vec<TargetOverdraw> = Vec::new();
    let none = Fetches::new();
    for d in f.draws.iter().filter(|d| d.raw.kind != DrawKind::Clear) {
        let p = pixels(f, d, &none);
        let key = d.registers.as_ref().map(|r| rsx_state::interpret(r)).map_or_else(|| "unknown".to_string(), |v| {
            let hex = |n: &str| v.surfaces.iter().find(|x| x.name == n).map_or("?".to_string(), |x| x.value.text());
            format!("color0 {} / zeta {} {}x{} {}", hex("color0.offset"), hex("zeta.offset"),
                uint_field(&v.surfaces, "clip.width").map_or("?".into(), |w| w.to_string()),
                uint_field(&v.surfaces, "clip.height").map_or("?".into(), |h| h.to_string()), hex("antialias"))
        });
        let i = match out.iter().position(|t| t.key == key) {
            Some(i) => i,
            None => {
                out.push(TargetOverdraw { key, draws: 0, counted: 0, samples: 0, area: p.area, overdraw: None });
                out.len() - 1
            }
        };
        let t = &mut out[i];
        t.draws += 1;
        if let Some(s) = p.samples {
            t.counted += 1;
            t.samples += u64::from(s);
        }
        if t.area.is_none() {
            t.area = p.area;
        }
    }
    for t in &mut out {
        t.overdraw = t.area.filter(|_| t.counted > 0).map(|a| t.samples as f64 / a as f64);
    }
    out
}

/// One draw's zcull picture: the stream's events, the bound region for its depth buffer and whether zcull can act.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ZcullAt {
    pub invalidated: bool,
    pub cleared: bool,
    /// The region (its latest record at or before this draw) whose offset is this draw's depth buffer.
    pub region: Option<ZcullRegion>,
    /// Some(true): a recorded region at the depth buffer's offset covers the draw's clip extent with the surface's
    /// zeta format, antialias mode and local memory, and depth cull is on; Some(false): such a region, but depth
    /// cull is off; None: not established (no such region recorded: binds before the profiler attached or from
    /// another thread are not seen; or the region does not match; or a needed register is unknown).
    pub effective: Option<bool>,
    pub reason: &'static str,
    /// A heuristic: the zcull direction disagrees with the depth function (LESS with GREATER/GEQUAL, or the reverse).
    pub direction_mismatch: Option<bool>,
}

pub fn zcull_at(f: &FrameView, d: &Draw) -> ZcullAt {
    let mut z = ZcullAt { invalidated: d.raw.flags & DF_ZCULL_INVALIDATED != 0,
        cleared: d.raw.flags & DF_ZCULL_CLEARED != 0, region: None, effective: None,
        reason: "no register state", direction_mismatch: None };
    if f.decoder < 5 {
        z.reason = "not recorded (decoder older than 5)";
        return z;
    }
    let Some(regs) = &d.registers else { return z };
    let v = rsx_state::interpret(regs);
    let field = |fs: &[rsx_state::Field], n: &str| fs.iter().find(|x| x.name == n).map(|x| x.value.clone());
    let mut latest: [Option<ZcullRegion>; ZCULL_REGIONS as usize] = [None; ZCULL_REGIONS as usize];
    for r in f.zcull_regions.iter().filter(|r| r.ordinal <= d.raw.ordinal) {
        latest[r.index as usize] = Some(*r);
    }
    let zeta = uint_field(&v.surfaces, "zeta.offset");
    let depth_on = match field(&v.render, "zcull.enable.depth") {
        Some(rsx_state::Value::Bool(b)) => Some(b),
        _ => None,
    };
    // The depth buffer as the draw uses it, from the raw registers: zeta offset and location, the surface format's
    // zeta format and antialias mode, and the clip extent the draw can write.
    let raw = |m: u32| regs.get(m);
    let rt = raw(0x208);
    let local = raw(0x198).and_then(|v| match v { 0xfeed_0000 => Some(true), 0xfeed_0001 => Some(false), _ => None });
    let clip = raw(0x200).zip(raw(0x204)).map(|(h, w)| ((h & 0xffff) + (h >> 16), (w & 0xffff) + (w >> 16)));
    let at_offset = zeta.and_then(|o| latest.iter().flatten().find(|r| r.bound && u64::from(r.offset) == o).copied());
    z.region = at_offset;
    // Coverage and compatibility: the region spans the clip extent, and has the surface's zeta format, antialias
    // mode and memory (zcull is local memory). Any of these unknown leaves coverage unknown.
    let fits = at_offset.map(|r| -> Option<bool> {
        let (x_end, y_end) = clip?;
        let rt = rt?;
        let zeta_format = (rt >> 5) & 7;
        let aa = (rt >> 12) & 0xf;
        Some(r.width >= x_end && r.height >= y_end && r.z_format == zeta_format && r.aa_format == aa && local?)
    });
    (z.effective, z.reason) = match (zeta, fits, depth_on) {
        (None, _, _) => (None, "the depth buffer offset is not known"),
        (_, Some(Some(true)), Some(true)) =>
            (Some(true), "a recorded zcull region covers this depth buffer (extent, format, antialias, local); depth cull is on"),
        (_, Some(Some(true)), Some(false)) =>
            (Some(false), "a recorded zcull region covers this depth buffer, but depth cull is off"),
        (_, Some(Some(true)), None) =>
            (None, "a recorded zcull region covers this depth buffer; the depth cull enable is not known"),
        (_, Some(Some(false)), _) =>
            (None, "the zcull region recorded at this depth buffer does not cover its extent or does not match its format, antialias mode or memory; coverage not established"),
        (_, Some(None), _) =>
            (None, "a zcull region is recorded at this depth buffer, but the surface's extent, format or memory is not known"),
        (_, None, _) =>
            (None, "no recorded zcull region at this depth buffer (a region bound before the profiler attached, or from another thread, is not seen)"),
    };
    let func = match field(&v.render, "depth.func") {
        Some(rsx_state::Value::Name(n)) => Some(n),
        _ => None,
    };
    let dir = match field(&v.render, "zcull.dir") {
        Some(rsx_state::Value::Name(n)) => Some(n),
        _ => None,
    };
    if let (Some(func), Some(dir)) = (func, dir) {
        let greater = func.starts_with("GREATER") || func.starts_with("GEQUAL");
        let less = func.starts_with("LESS") || func.starts_with("LEQUAL");
        z.direction_mismatch = Some((dir == "LESS" && greater) || (dir == "GREATER" && less));
    }
    z
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
                  by: Option<GroupBy>, folder: Option<&Folder>) -> io::Result<()> {
    let Some(inv) = &cap.rsx_frame else {
        return writeln!(out, "rsx-frame: capability absent (file_flags bit 10 clear)");
    };
    if let Some(c) = &inv.config {
        writeln!(out, "rsx-frame: {}; staging {} x {} bytes; programs {} labels {} x {} bytes", c.availability.text(),
            c.blocks, c.block_bytes, c.program_capacity, c.label_capacity, c.label_bytes)?;
    } else {
        writeln!(out, "rsx-frame: no RSXF_CONFIG yet")?;
    }
    if let Some(d) = folder {
        let s = &d.scan;
        writeln!(out, "shader folder {}: {} containers read ({} bytes), {} programs, {} invalid{}", d.path, s.files,
            s.bytes, s.names.len(), s.invalid, s.limit.as_ref().map_or_else(String::new, |l| format!("; stopped: {l}")))?;
    }
    if frames.is_empty() {
        return writeln!(out, "no RSX frame captures");
    }
    let empty = Folder::default();
    let names = &folder.unwrap_or(&empty).scan.names;
    let fetches = &folder.unwrap_or(&empty).scan.fetches;
    for f in frames {
        writeln!(out, "capture {} \"{}\": owner {} frame {} window {} source {} timing {}; {}", f.id, f.name, f.owner,
            f.frame, f.window, if f.source == Source::Live { format!("live request {}", f.request_id) } else { "app".into() },
            match (f.half, f.isolated) {
                (Half::First, _) => "PIPELINED (reached-marker intervals), frame 1 of 2",
                (Half::Second, _) => "ISOLATED, frame 2 of 2",
                (_, true) => "ISOLATED",
                _ => "PIPELINED (reached-marker intervals)",
            }, f.status.text())?;
        if f.half == Half::Second {
            let unpaired = f.pairs.iter().filter(|p| p.first.is_none() || p.second.is_none()).count();
            writeln!(out, "  paired with frame 1: {} pairs, {unpaired} unpaired", f.pairs.len() - unpaired)?;
        }
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
        for (i, p) in f.programs.iter().enumerate() {
            writeln!(out, "  program {} {}{}{}: {} instructions, {} bytes, {} constant blocks, {}, first at {:#x} (draw {}){}{}",
                p.index, p.id_text(), p.name.as_ref().map_or_else(String::new, |n| format!(" \"{n}\"")),
                if p.unreadable { None } else { names.get(&(p.profile, p.hash)) }
                    .map_or_else(String::new, |n| format!(" file {n}")), p.instructions,
                p.ucode_bytes, p.const_blocks, p.source.name(), p.address, p.first_draw,
                if p.unterminated { ", UNTERMINATED" } else { "" },
                if p.not_revalidated { ", bound before capture, not revalidated" } else { "" })?;
            writeln!(out, "    texture fetches: {}", fetch_text(program_fetches(f, i, fetches)))?;
        }
        if let Some(k) = draw {
            match f.draws.iter().find(|d| d.raw.ordinal == k) {
                Some(d) => write_draw(out, f, d, names, fetches)?,
                None => writeln!(out, "  draw {k}: not recorded in this capture")?,
            }
            continue;
        }
        if let Some(by) = by {
            for g in group(f, by, names) {
                writeln!(out, "  group {}: {} draws (timed {}, untimed {}), {} raw, {}", g.key, g.draws.len(), g.timed,
                    g.untimed, g.sum, pct(g.sum, t.sum))?;
            }
            continue;
        }
        if f.decoder >= 5 {
            for t in overdraw(f) {
                writeln!(out, "  target {}: {} draws ({} counted), {} samples{}", t.key, t.draws, t.counted, t.samples,
                    t.overdraw.map_or_else(String::new, |o| format!(", overdraw {o:.2}")))?;
            }
        }
        writeln!(out, "  #  kind prim count instances? tag label vp fp pass time gap tex smp")?;
        for d in &f.draws {
            let df = draw_fetches(f, d, fetches);
            let px = pixels(f, d, fetches);
            writeln!(out, "  {} {} {} {} tag {:#x} label {} vp {} fp {} pass {} time {} gap {} tex {} smp {}{}", d.raw.ordinal,
                d.raw.kind.name(), primitive_name(d.raw.mode), d.raw.count, d.raw.tag,
                d.label.as_deref().unwrap_or("-"),
                d.vp.map_or_else(|| "-".into(), |i| program_name(&f.programs[i], names)),
                d.fp.map_or_else(|| "-".into(), |i| program_name(&f.programs[i], names)),
                d.pass.map_or("-", |p| f.passes[p].name.as_str()), d.timing.text(),
                d.gap.map_or_else(|| "-".into(), |g| g.to_string()), df.text(),
                px.samples.map_or_else(|| "-".into(), |s| s.to_string()),
                if d.op_mismatch || d.raw.flags & DF_MISMATCH != 0 { " MISMATCH" } else { "" })?;
        }
    }
    Ok(())
}

fn write_draw(out: &mut dyn Write, f: &FrameView, d: &Draw, names: &BTreeMap<(Profile, u64), String>,
              fetches: &Fetches) -> io::Result<()> {
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
    let df = draw_fetches(f, d, fetches);
    writeln!(out, "    texture fetches: {} (static, per program run); vp {}; fp {}", df.text(),
        fetch_text(df.vp), fetch_text(df.fp))?;
    let px = pixels(f, d, fetches);
    match px.samples {
        Some(s) => writeln!(out, "    pixels: {s} passed samples (counting {}){}{}{}; early-z {}: {}", px.state.name(),
            px.per_pixel.map_or_else(String::new, |p| format!(", {p:.3} of the target")),
            px.raw_per_sample.map_or_else(String::new, |r| format!(", {r:.2} raw ticks per sample (isolated)")),
            px.shading_proxy.map_or_else(String::new, |w| format!(", {w} sample-instructions")),
            px.early_z.name(), px.early_z.shading())?,
        None => writeln!(out, "    pixels: not counted: {}",
            px.state.why_uncounted().unwrap_or("the draw was not timed"))?,
    }
    let z = zcull_at(f, d);
    writeln!(out, "    zcull: {}{}{}{}", z.reason,
        z.region.map_or_else(String::new, |r| format!(" (region {})", r.index)),
        if z.invalidated { "; invalidated before this draw" } else { "" },
        if z.cleared { "; zcull surface cleared before this draw" } else { "" })?;
    if z.direction_mismatch == Some(true) {
        writeln!(out, "    zcull: direction disagrees with the depth function (heuristic)")?;
    }
    writeln!(out, "    seen by: {}{}{}; words {}; caller {:#x}; tb {}", if d.hooked() { "hook " } else { "" },
        if d.decoded() { "stream" } else { "" }, if r.flags & DF_STATE_UNKNOWN != 0 { " (state unknown)" } else { "" },
        r.words, r.caller, r.tb)?;
    if let Some(regs) = &d.registers {
        let v = rsx_state::interpret(regs);
        let known = |fs: &[rsx_state::Field]| fs.iter().filter(|f| f.value != rsx_state::Value::Unknown)
            .map(|f| format!("{}={}", f.name, f.value.text())).collect::<Vec<_>>().join(" ");
        writeln!(out, "    state: {} registers known{}", regs.known_count(),
            if r.state_reset { ", decoder reset before this draw" } else { "" })?;
        if !d.changed.is_empty() {
            writeln!(out, "    changed: {}", d.changed.iter().map(|&m| rsx_state::register_name(m))
                .collect::<Vec<_>>().join(" "))?;
        }
        writeln!(out, "    render: {}", known(&v.render))?;
        writeln!(out, "    surfaces: {}", known(&v.surfaces))?;
        writeln!(out, "    index: {}", known(&v.index))?;
        let slots = texture_slots(d, df.fp.as_ref().map(|(t, _)| t)).unwrap_or_default();
        let stale = slots.iter().filter(|s| s.bound == Bound::Disabled && s.fetches.unwrap_or(0) == 0
            && v.textures[s.slot as usize].iter().any(|f| f.name == "format" && f.value != rsx_state::Value::Unknown))
            .count();
        for s in &slots {
            if s.enabled != Some(false) || s.fetches.unwrap_or(0) > 0 {
                writeln!(out, "    texture {}: {}{}; {}", s.slot, s.bound.name(),
                    s.fetches.map_or_else(String::new, |n| format!(", {n} fetches")), known(&v.textures[s.slot as usize]))?;
            }
        }
        if stale > 0 {
            writeln!(out, "    textures: {stale} disabled slots still hold a format (stale, not bound)")?;
        }
        for (i, a) in v.attributes.iter().enumerate() {
            if a.iter().any(|f| f.name == "enabled" && f.value == rsx_state::Value::Bool(true)) {
                writeln!(out, "    attribute {i}: {}", known(a))?;
            }
        }
    }
    Ok(())
}

fn program_json(f: &FrameView, i: usize, folder: &Folder) -> Value {
    let p = &f.programs[i];
    let mut v = json!({"index": p.index, "profile": p.profile.short(), "hash": format!("{:016x}", p.hash), "id": p.id_text(),
        "name": p.name, "source": p.source.name(), "instructions": p.instructions, "ucode_bytes": p.ucode_bytes,
        "const_blocks": p.const_blocks, "address": p.address, "first_draw": p.first_draw,
        "unterminated": p.unterminated, "unreadable": p.unreadable, "not_revalidated": p.not_revalidated,
        "uniforms": p.uniforms.iter().map(|u| json!({"name": u.name, "resource": u.resource, "count": u.count,
            "kind": if u.kind == UniformKind::VpConstant { "vp_constant" } else { "fp_embedded" }})).collect::<Vec<_>>(),
        "uniforms_truncated": p.uniforms_truncated});
    let o = v.as_object_mut().expect("object");
    o.insert("shader_file".into(), json!(if p.unreadable { None } else { folder.scan.names.get(&(p.profile, p.hash)) }));
    o.insert("texture_fetches".into(), fetch_json(program_fetches(f, i, &folder.scan.fetches)));
    v
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

fn draw_json(f: &FrameView, d: &Draw, folder: &Folder) -> Value {
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
    let px = pixels(f, d, &folder.scan.fetches);
    o.insert("pixels".into(), json!({"state": px.state.name(), "samples": px.samples, "area": px.area,
        "per_pixel": px.per_pixel, "raw_per_sample": px.raw_per_sample, "early_z": px.early_z.name(),
        "shading": (d.raw.kind != DrawKind::Clear).then(|| px.early_z.shading()),
        "shading_proxy": px.shading_proxy, "fetch_proxy": px.fetch_proxy,
        "uncounted": px.samples.is_none().then(|| px.state.why_uncounted().unwrap_or("the draw was not timed")),
        "zpass_delta": d.zpass_delta}));
    let z = zcull_at(f, d);
    o.insert("zcull".into(), json!({"invalidated": z.invalidated, "cleared": z.cleared,
        "region": z.region.map(|r| r.index), "effective": z.effective, "reason": z.reason,
        "direction_mismatch": z.direction_mismatch}));
    let df = draw_fetches(f, d, &folder.scan.fetches);
    o.insert("texture_fetches".into(), json!({"vp": df.vp.map(|(t, _)| t.total()), "fp": df.fp.map(|(t, _)| t.total()),
        "total": df.total(), "exact": df.exact()}));
    if let Some(slots) = texture_slots(d, df.fp.as_ref().map(|(t, _)| t)) {
        o.insert("texture_slots".into(), Value::Array(slots.iter().map(|s| json!({"slot": s.slot,
            "enabled": s.enabled, "fetches": s.fetches, "bound": s.bound.flag(), "state": s.bound.name(),
            "reason": s.bound.reason()})).collect()));
    }
    if let Some(regs) = &d.registers {
        o.insert("state".into(), rsx_state::view_json(&rsx_state::interpret(regs)));
        o.insert("changed".into(), json!(d.changed.iter().map(|&m| rsx_state::register_name(m)).collect::<Vec<_>>()));
        o.insert("state_reset".into(), json!(r.state_reset));
    }
    if d.vp_constants.is_some() {
        let fl = |v: &[Option<f32>; 4]| json!(v.map(|c| c.map(|x| json!(x)).unwrap_or(Value::Null)));
        let us = uniforms(f, d);
        let vp: Vec<Value> = us.iter().filter(|u| u.profile == Profile::Vertex).map(|u| json!({"name": u.name,
            "slot": u.resource, "values": u.values.iter().map(fl).collect::<Vec<_>>()})).collect();
        let fp: Vec<Value> = us.iter().filter(|u| u.profile == Profile::Fragment).map(|u| json!({"name": u.name,
            "offset": u.resource, "values": u.values.iter().map(fl).collect::<Vec<_>>(),
            "provenance": u.provenance.map(|p| p.map(Provenance::name))})).collect();
        let list: Vec<Value> = d.fp_constants.as_ref().map_or_else(Vec::new, |c| c.entries.iter().map(|e| json!({
            "offset": e.offset, "provenance": e.provenance.map(Provenance::name), "value": e.floats()})).collect());
        o.insert("constants".into(), json!({"vp": vp, "fp": fp, "vp_changed": d.vpc_changed, "fp_list": list}));
        o.insert("run".into(), json!(d.run));
    }
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
    o.insert("baseline_registers".into(), json!(e.baseline_registers));
    o.insert("state_records".into(), json!(e.state_records));
    o.insert("vpconst_records".into(), json!(e.vpconst_records));
    o.insert("fpconst_records".into(), json!(e.fpconst_records));
    o.insert("uniform_records".into(), json!(e.uniform_records));
    o.insert("ucode_records".into(), json!(e.ucode_records));
    o.insert("decoder_reject".into(),
        json!(e.decoder_reject.map(|(h, r)| json!({"header": h, "reason": auto_rsx::reject_reason(r)}))));
    v
}

fn frame_json(f: &FrameView, folder: &Folder) -> Value {
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
    o.insert("half".into(), json!(match f.half { Half::Only => "only", Half::First => "first", Half::Second => "second" }));
    o.insert("pairs".into(), Value::Array(f.pairs.iter().map(|p| json!({"first": p.first, "second": p.second}))
        .collect()));
    o.insert("program_bytes".into(), Value::Array(f.programs.iter().enumerate().map(|(i, p)| {
        let b = f.bytes_of(i);
        json!({"index": p.index, "bytes": b.map(|b| b.bytes.len()), "total": b.map(|b| b.total),
            "complete": b.map(|b| b.complete)})
    }).collect()));
    o.insert("instance_runs".into(), Value::Array(f.instance_runs.iter().map(|r| {
        let vp_names: Vec<String> = f.draws[r.first].vp.map_or_else(Vec::new, |i| f.programs[i].uniforms.iter()
            .filter(|u| r.vp_slots.iter().any(|&s| (u.resource..u.resource + u.count).contains(&u32::from(s))))
            .map(|u| u.name.clone()).collect());
        json!({"first": f.draws[r.first].raw.ordinal, "length": r.len, "vp_slots": r.vp_slots,
            "fp_offsets": r.fp_offsets, "vp_uniforms": vp_names})
    }).collect()));
    o.insert("programs".into(), Value::Array((0..f.programs.len()).map(|i| program_json(f, i, folder)).collect()));
    o.insert("passes".into(), Value::Array(passes));
    o.insert("draws".into(), Value::Array(f.draws.iter().map(|d| draw_json(f, d, folder)).collect()));
    o.insert("pixels_forced".into(), json!(f.pixels_forced));
    o.insert("zcull_regions".into(), Value::Array(f.zcull_regions.iter().map(|r| json!({"index": r.index,
        "ordinal": r.ordinal, "bound": r.bound, "before_capture": r.before_capture, "offset": r.offset,
        "width": r.width, "height": r.height, "cull_start": r.cull_start, "z_format": r.z_format,
        "aa_format": r.aa_format, "dir": r.dir, "format": r.format, "s_func": r.s_func, "s_ref": r.s_ref,
        "s_mask": r.s_mask})).collect()));
    o.insert("overdraw".into(), Value::Array(overdraw(f).iter().map(|t| json!({"target": t.key, "draws": t.draws,
        "counted": t.counted, "samples": t.samples, "area": t.area, "overdraw": t.overdraw})).collect()));
    o.insert("end".into(), f.end.as_ref().map_or(Value::Null, end_json));
    v
}

/// The JSON document of `ps3dk-prof rsx-frame --json` (schema 1), with no shader folder.
pub fn to_json(cap: &Capture, frames: &[FrameView]) -> Value {
    to_json_with(cap, frames, None)
}

/// [`to_json`] with programs named and counted from a shader folder (`--shader-dir`).
pub fn to_json_with(cap: &Capture, frames: &[FrameView], folder: Option<&Folder>) -> Value {
    let cfg = cap.rsx_frame.as_ref().and_then(|i| i.config.as_ref());
    let empty = Folder::default();
    let dir = folder.unwrap_or(&empty);
    json!({
        "schema": 1,
        "capability": cap.rsx_frame.is_some(),
        "availability": cfg.map(|c| c.availability.text()),
        "decoder": cfg.map(|c| c.decoder),
        "shader_dir": folder.map(|d| json!({"path": d.path, "files": d.scan.files, "bytes": d.scan.bytes,
            "programs": d.scan.names.len(), "invalid": d.scan.invalid, "limit": d.scan.limit})),
        "captures": Value::Array(frames.iter().map(|f| frame_json(f, dir)).collect()),
    })
}

/// The static texture fetches of one draw: its VP's and its FP's (each from [`program_fetches`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrawFetches {
    pub vp: Option<(nv40_disasm::TexFetches, FetchOrigin)>,
    pub fp: Option<(nv40_disasm::TexFetches, FetchOrigin)>,
}
impl DrawFetches {
    /// VP + FP fetch instructions; None unless both programs' counts are known.
    pub fn total(&self) -> Option<u32> {
        Some(self.vp?.0.total() + self.fp?.0.total())
    }
    /// Both counts known and exact.
    pub fn exact(&self) -> bool {
        self.vp.is_some_and(|(t, _)| t.exact()) && self.fp.is_some_and(|(t, _)| t.exact())
    }
    /// `3`, `3+` (a lower bound) or `-` (unknown).
    pub fn text(&self) -> String {
        match self.total() {
            None => "-".into(),
            Some(n) if self.exact() => n.to_string(),
            Some(n) => format!("{n}+"),
        }
    }
}

/// The static texture fetches of draw `d`'s programs.
pub fn draw_fetches(f: &FrameView, d: &Draw, folder: &Fetches) -> DrawFetches {
    DrawFetches { vp: d.vp.and_then(|i| program_fetches(f, i, folder)), fp: d.fp.and_then(|i| program_fetches(f, i, folder)) }
}

/// The text of a program's fetch counts.
pub fn fetch_text(t: Option<(nv40_disasm::TexFetches, FetchOrigin)>) -> String {
    let Some((t, origin)) = t else { return "unknown (no program bytes)".into() };
    let units = t.units();
    format!("{} {}{}{} ({})", t.total(), t.ops_text(),
        if units.is_empty() { String::new() } else {
            format!("; units {}", units.iter().map(|u| u.to_string()).collect::<Vec<_>>().join(","))
        },
        if t.exact() { "" } else if !t.terminated { "; lower bound: no end bit" } else { "; lower bound: unknown opcodes" },
        origin.name())
}

fn fetch_json(t: Option<(nv40_disasm::TexFetches, FetchOrigin)>) -> Value {
    let Some((t, origin)) = t else { return Value::Null };
    let mut by_op = serde_json::Map::new();
    for (o, n) in nv40_disasm::TEX_OPS.iter().zip(t.by_op) {
        by_op.insert((*o).into(), json!(n));
    }
    let mut by_unit = serde_json::Map::new();
    for u in t.units() {
        by_unit.insert(u.to_string(), json!(t.by_unit[u as usize]));
    }
    json!({"source": match origin { FetchOrigin::Capture => "capture", FetchOrigin::ShaderFolder => "shader_folder" },
        "total": t.total(), "by_op": by_op, "by_unit": by_unit, "instructions": t.instructions,
        "unknown_instructions": t.unknown, "terminated": t.terminated, "exact": t.exact()})
}

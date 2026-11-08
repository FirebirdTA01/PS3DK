//! RSX frame captures ("Capture RSX frame", phase B1): the reader side.
//!
//! A capture is one frame of a live app recorded on the RSX at draw granularity.  The runtime writes it into an
//! ordinary extension file as GPU events of one RSX domain:
//!
//! - three kinds of MARK markers whose descriptor names are reserved ([`NAME_BEGIN`], [`NAME_CKPT`], [`NAME_END`]) and
//!   whose logical tags carry the capture's identity and receipt counts in a strict v1 grammar ([`parse_tag`]);
//! - one GPU zone (BEGIN/END pair) per instrumented draw, labelled by a fixed draw-kind descriptor: its name is the
//!   kind and its logical tag is exactly `ps3dk.rsxcap.v1 kind=<kind>` ([`DrawKind`], [`descriptor_class`]).  A
//!   zone whose descriptor only has a draw-kind NAME is an ordinary app zone, not a draw.
//!
//! [`rsx_captures`] finds every capture in an [`Analysis`], groups its markers by `cap`, attributes draw pairs to it
//! by sequence number, splits them into 4096-draw blocks at the checkpoints, and classifies it as FINALIZED, PARTIAL
//! (with every reason counted) or ANOMALY.  Anything unexpected in the reserved-marker namespace is reported as an
//! [`Anomaly`]; nothing is silently ignored.  The live link's request state ([`AckState`]) is a separate input
//! ([`request_view`]) and never changes what the data says.
//!
//! Only evidence the extension reader accepted is used: markers are the MARK events it recorded in `raw.marks` (a
//! reserved MARK it refused, or any MARK after its terminal PROTOCOL ERROR, is an untrusted-marker fault), and draws
//! are its `raw.pairs`; draw events it could not pair count as unpaired.  Any namespace fault at a sequence number
//! inside a capture's interval taints that capture (FINALIZED is then impossible).
//!
//! Work and scratch are bounded.  Work: a constant number of linear passes over each GPU source's events, pairs,
//! LOSS records and accepted marks (arrays the reader already holds), with O(log) lookups against at most
//! [`MAX_CAPTURES`] intervals of at most [`MAX_CHECKPOINTS`] + 2 markers each, plus, per capture whose stored draws
//! are not already in BEGIN order, one in-place O(D log D) sort of its D <= [`DRAW_CAP`] stored indices.  Scratch: one
//! class per descriptor (bounded by the reader's descriptor limit), the set of caps named in the file and a small
//! validation record per cap beyond the analysis bound (both bounded by the descriptor count, since every marker is
//! its own descriptor), at most [`MAX_CAPTURES`] capture records, at most [`MAX_ANOMALIES`] anomalies and
//! [`MAX_LISTED_CAPS`] listed caps, and the stored draw indices: a capture's in-interval draw pairs are
//! COUNTED first without allocating, then charged to the per-capture [`DRAW_CAP`] and to the file's
//! [`Budgets::stored_draws`] (default [`MAX_STORED_DRAWS`]) before one exact allocation; a capture over either is
//! refused (PARTIAL, over cap / draw budget) with no draw stored, and sorting is in place.  No durations: raw RSX
//! timestamps only (mapping is UNMEASURED here).

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

use crate::analysis::Analysis;
use crate::ext::{RawEvent, SourceKey, SourceReport, EV_MARK, STATE_ABANDONED, STATE_FINALIZED, STATE_OVERFLOWED};

/// The version token every v1 tag starts with.
pub const TAG_VERSION: &str = "ps3dk.rsxcap.v1";
/// The namespace prefix of every capture tag (any version).
pub const TAG_PREFIX: &str = "ps3dk.rsxcap.";
pub const NAME_BEGIN: &str = "rsx.capture.begin";
pub const NAME_CKPT: &str = "rsx.capture.ckpt";
pub const NAME_END: &str = "rsx.capture.end";
/// The END tag's coverage statement.
pub const COVERAGE: &str = "librsx-draws-v1";
/// Draws per block: block k holds ordinals [4096k, 4096k + 4095].
pub const BLOCK_DRAWS: u32 = 4096;
/// The per-capture draw cap: a capture with more draw pairs is refused (PARTIAL, over cap), never stored.
pub const DRAW_CAP: u32 = 262_144;
/// Upper bound of `hooks` and of the five side counters (`unsafe`, `no_space`, `reentrant`, `invalid`, `mismatch`).
pub const SIDE_MAX: u32 = 262_144;
/// Blocks per capture at the cap (64); a checkpoint names block 0..=62.
pub const MAX_BLOCKS: u32 = DRAW_CAP / BLOCK_DRAWS;
/// Captures analysed per file; more are listed by cap only, with a "too many captures" anomaly.
pub const MAX_CAPTURES: usize = 16;
/// Anomalies kept per file; the rest are counted in [`RsxCaptures::anomalies_omitted`].
pub const MAX_ANOMALIES: usize = 256;
/// Caps beyond [`MAX_CAPTURES`] listed by number; the rest are counted in [`RsxCaptures::not_analysed_omitted`].
pub const MAX_LISTED_CAPS: usize = 256;
/// The default file-wide ceiling on stored draw indices (u32 each: 4 MiB): four captures at the cap.
pub const MAX_STORED_DRAWS: u64 = 4 * DRAW_CAP as u64;

/// The reader's scratch budget.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Budgets {
    /// Draw indices stored over all captures of a file, charged per capture (ascending cap) before allocation.  A
    /// capture that would exceed it is refused: PARTIAL with [`Reason::DrawBudget`], no draw listed.
    pub stored_draws: u64,
}

impl Default for Budgets {
    fn default() -> Self {
        Budgets { stored_draws: MAX_STORED_DRAWS }
    }
}
/// Checkpoints kept per capture (63 are needed at the cap; one spare).
pub const MAX_CHECKPOINTS: usize = MAX_BLOCKS as usize;
/// The longest tag each grammar admits (every field at its largest value): BEGIN 121, CHECKPOINT 107, END 490 bytes.
/// (The writer's END buffer bound of 494 assumes a 10-digit `hooks`; the grammar caps `hooks` at 262144.)
pub const MAX_BEGIN_LEN: usize = 121;
pub const MAX_CKPT_LEN: usize = 107;
pub const MAX_END_LEN: usize = 490;

// ---- draw kinds ---------------------------------------------------------------------------------------------------

/// The fixed draw-kind descriptors a capture's draw zones use.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum DrawKind {
    Arrays,
    Index,
    Inline,
    Clear,
    ClearZcull,
}

impl DrawKind {
    pub const ALL: [DrawKind; 5] = [DrawKind::Arrays, DrawKind::Index, DrawKind::Inline, DrawKind::Clear, DrawKind::ClearZcull];

    /// The descriptor name.
    pub fn name(self) -> &'static str {
        match self {
            DrawKind::Arrays => "draw.arrays",
            DrawKind::Index => "draw.index",
            DrawKind::Inline => "draw.inline",
            DrawKind::Clear => "clear",
            DrawKind::ClearZcull => "clear.zcull",
        }
    }

    pub fn from_name(name: &str) -> Option<DrawKind> {
        DrawKind::ALL.into_iter().find(|k| k.name() == name)
    }

    /// The reserved logical tag of this kind's descriptor: `ps3dk.rsxcap.v1 kind=<name>`.
    pub fn tag(self) -> String {
        format!("{TAG_VERSION} kind={}", self.name())
    }
}

/// Parses a draw-kind tag: exactly `ps3dk.rsxcap.v1 kind=<kind>` (single space, nothing else) with a known kind.
pub fn parse_draw_tag(tag: &str) -> Result<DrawKind, String> {
    let mut it = tag.split(' ');
    match it.next() {
        Some(TAG_VERSION) => {}
        Some(v) if v.starts_with(TAG_PREFIX) => return Err(format!("version token {v:?}, want {TAG_VERSION}")),
        _ => return Err(format!("does not start with {TAG_VERSION}")),
    }
    let Some(tok) = it.next() else { return Err("missing kind=".into()) };
    let Some(k) = tok.strip_prefix("kind=") else {
        return Err(if tok.is_empty() {
            "empty field (spaces must be single)".into()
        } else {
            format!("expected kind=, found {tok:?}")
        });
    };
    let kind = DrawKind::from_name(k).ok_or_else(|| format!("unknown draw kind {k:?}"))?;
    match it.next() {
        None => Ok(kind),
        Some("") => Err("trailing space".into()),
        Some(t) => Err(format!("unexpected field {t:?} after kind=")),
    }
}

/// What a descriptor is in the capture namespace, from its name and logical tag.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DescClass {
    /// A reserved marker name (its tag is parsed by [`parse_tag`]).
    Marker(MarkerKind),
    /// A genuine draw-kind descriptor: tag `ps3dk.rsxcap.v1 kind=<k>` AND name equal to `<k>`.
    Draw(DrawKind),
    /// A draw-kind descriptor gone wrong: a malformed or unknown-kind draw tag, a name/tag mismatch, or a draw-kind
    /// name carrying a non-draw capture tag.
    DrawFault(String),
    /// Some other `ps3dk.rsxcap.` tag under a non-reserved name.
    ForeignTag,
    /// Outside the namespace (an app descriptor, whatever its name).
    Plain,
}

/// Classifies a descriptor.  The runtime must register draw-kind descriptors exactly as [`DrawKind::tag`] with
/// the kind as the name; nothing else is a draw.
pub fn descriptor_class(name: &str, tag: &str) -> DescClass {
    if let Some(k) = MarkerKind::of_name(name) {
        return DescClass::Marker(k);
    }
    if !tag.starts_with(TAG_PREFIX) {
        return DescClass::Plain;
    }
    let draw_like = tag.split(' ').nth(1).is_some_and(|t| t.starts_with("kind")) || DrawKind::from_name(name).is_some();
    if !draw_like {
        return DescClass::ForeignTag;
    }
    match parse_draw_tag(tag) {
        Ok(k) if k.name() == name => DescClass::Draw(k),
        Ok(k) => DescClass::DrawFault(format!("name {name:?} with a tag of kind {}", k.name())),
        Err(e) => DescClass::DrawFault(e),
    }
}

// ---- tags ---------------------------------------------------------------------------------------------------------

/// The three reserved marker kinds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MarkerKind {
    Begin,
    Checkpoint,
    End,
}

impl MarkerKind {
    /// The kind a reserved descriptor name stands for.
    pub fn of_name(name: &str) -> Option<MarkerKind> {
        match name {
            NAME_BEGIN => Some(MarkerKind::Begin),
            NAME_CKPT => Some(MarkerKind::Checkpoint),
            NAME_END => Some(MarkerKind::End),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            MarkerKind::Begin => NAME_BEGIN,
            MarkerKind::Checkpoint => NAME_CKPT,
            MarkerKind::End => NAME_END,
        }
    }

    pub fn word(self) -> &'static str {
        match self {
            MarkerKind::Begin => "BEGIN",
            MarkerKind::Checkpoint => "CHECKPOINT",
            MarkerKind::End => "END",
        }
    }
}

/// A capture's identity, carried by BEGIN and END.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Identity {
    pub cap: u32,
    pub conn: u32,
    pub wire: u32,
    pub dom: u32,
    pub owner: u32,
    pub frame: u64,
}

/// A block's submission receipt counts (its zone BEGIN and END receipts together).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Receipts {
    pub ev: u32,
    pub lost: u32,
    pub sup: u32,
    pub refused: u32,
}

impl Receipts {
    fn clean(&self) -> bool {
        self.lost == 0 && self.sup == 0 && self.refused == 0
    }
}

/// A CHECKPOINT tag: emitted before the first draw of block `block + 1`, with the counts of block `block`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Checkpoint {
    pub cap: u32,
    pub block: u32,
    pub counts: Receipts,
}

/// An END tag: the identity, the last block's counts and the capture's totals.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct EndTag {
    pub id: Identity,
    pub block: u32,
    pub counts: Receipts,
    pub hooks: u32,
    pub capped: bool,
    pub other_ctx: u32,
    pub other_ctx_sat: bool,
    pub other_thread: u32,
    pub other_thread_sat: bool,
    pub immediate: u32,
    pub immediate_sat: bool,
    pub ckpt_missing: u32,
    pub unsafe_draws: u32,
    pub no_space: u32,
    pub reentrant: u32,
    pub invalid: u32,
    pub mismatch: u32,
    pub counts_sat: bool,
    pub hook_fault: bool,
}

/// A decoded v1 tag.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tag {
    Begin(Identity),
    Checkpoint(Checkpoint),
    End(EndTag),
}

impl Tag {
    pub fn kind(&self) -> MarkerKind {
        match self {
            Tag::Begin(_) => MarkerKind::Begin,
            Tag::Checkpoint(_) => MarkerKind::Checkpoint,
            Tag::End(_) => MarkerKind::End,
        }
    }
    pub fn cap(&self) -> u32 {
        match self {
            Tag::Begin(i) => i.cap,
            Tag::Checkpoint(c) => c.cap,
            Tag::End(e) => e.id.cap,
        }
    }
    /// The `dom` field (CHECKPOINT has none).
    pub fn dom(&self) -> Option<u32> {
        match self {
            Tag::Begin(i) => Some(i.dom),
            Tag::Checkpoint(_) => None,
            Tag::End(e) => Some(e.id.dom),
        }
    }
}

#[derive(Clone, Copy)]
enum F {
    U32,
    U64,
    Flag,
    /// <= SIDE_MAX
    Bounded,
    Coverage,
}

const BEGIN_FIELDS: &[(&str, F)] =
    &[("cap", F::U32), ("conn", F::U32), ("wire", F::U32), ("dom", F::U32), ("owner", F::U32), ("frame", F::U64)];
const CKPT_FIELDS: &[(&str, F)] =
    &[("cap", F::U32), ("block", F::U32), ("ev", F::U32), ("lost", F::U32), ("sup", F::U32), ("ref", F::U32)];
const END_FIELDS: &[(&str, F)] = &[
    ("cap", F::U32),
    ("conn", F::U32),
    ("wire", F::U32),
    ("dom", F::U32),
    ("owner", F::U32),
    ("frame", F::U64),
    ("block", F::U32),
    ("ev", F::U32),
    ("lost", F::U32),
    ("sup", F::U32),
    ("ref", F::U32),
    ("hooks", F::Bounded),
    ("capped", F::Flag),
    ("other_ctx", F::U32),
    ("other_ctx_sat", F::Flag),
    ("other_thread", F::U32),
    ("other_thread_sat", F::Flag),
    ("immediate", F::U32),
    ("immediate_sat", F::Flag),
    ("ckpt_missing", F::U32),
    ("unsafe", F::Bounded),
    ("no_space", F::Bounded),
    ("reentrant", F::Bounded),
    ("invalid", F::Bounded),
    ("mismatch", F::Bounded),
    ("counts_sat", F::Flag),
    ("hook_fault", F::Flag),
    ("coverage", F::Coverage),
];

fn value(key: &str, ty: F, v: &str) -> Result<u64, String> {
    if let F::Coverage = ty {
        return if v == COVERAGE { Ok(0) } else { Err(format!("coverage={v:?}, want {COVERAGE}")) };
    }
    if v.is_empty() || !v.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("{key}={v:?} is not plain decimal digits"));
    }
    if v.len() > 1 && v.starts_with('0') {
        return Err(format!("{key}={v} has a leading zero"));
    }
    let n: u64 = if v.len() > 20 { None } else { v.parse().ok() }.ok_or_else(|| format!("{key}={v} overflows"))?;
    let max = match ty {
        F::U32 => u64::from(u32::MAX),
        F::U64 => u64::MAX,
        F::Flag => 1,
        F::Bounded => u64::from(SIDE_MAX),
        F::Coverage => unreachable!(),
    };
    if n > max {
        return Err(format!("{key}={n} exceeds {max}"));
    }
    Ok(n)
}

fn fields(spec: &[(&str, F)], tag: &str) -> Result<Vec<u64>, String> {
    let mut it = tag.split(' ');
    match it.next() {
        Some(TAG_VERSION) => {}
        Some(v) if v.starts_with(TAG_PREFIX) => return Err(format!("version token {v:?}, want {TAG_VERSION}")),
        _ => return Err(format!("does not start with {TAG_VERSION}")),
    }
    let mut out = Vec::with_capacity(spec.len());
    for &(key, ty) in spec {
        let Some(tok) = it.next() else { return Err(format!("missing {key}=")) };
        if tok.is_empty() {
            return Err(format!("empty field before {key}= (spaces must be single)"));
        }
        let Some(v) = tok.strip_prefix(key).and_then(|r| r.strip_prefix('=')) else {
            return Err(format!("expected {key}=, found {tok:?}"));
        };
        out.push(value(key, ty, v)?);
    }
    match it.next() {
        None => Ok(out),
        Some("") => Err("trailing space".into()),
        Some(tok) => Err(format!("unexpected field {tok:?} after the last key")),
    }
}

fn u32_(v: u64) -> u32 {
    u32::try_from(v).expect("range-checked")
}

/// Parses a tag of the given kind under the v1 grammar: the version token, then exactly the kind's keys in their
/// fixed order, separated by single spaces, values plain decimal (no sign, no leading zero except "0"), u32 fields
/// at most u32::MAX, `frame` a u64, flags 0 or 1, `hooks` and the five side counters at most 262144, the END's
/// coverage exactly [`COVERAGE`], and nothing else.  Returns the reason on refusal.
pub fn parse_tag(kind: MarkerKind, tag: &str) -> Result<Tag, String> {
    let id = |v: &[u64]| Identity {
        cap: u32_(v[0]),
        conn: u32_(v[1]),
        wire: u32_(v[2]),
        dom: u32_(v[3]),
        owner: u32_(v[4]),
        frame: v[5],
    };
    let counts = |v: &[u64]| Receipts { ev: u32_(v[0]), lost: u32_(v[1]), sup: u32_(v[2]), refused: u32_(v[3]) };
    Ok(match kind {
        MarkerKind::Begin => Tag::Begin(id(&fields(BEGIN_FIELDS, tag)?)),
        MarkerKind::Checkpoint => {
            let v = fields(CKPT_FIELDS, tag)?;
            Tag::Checkpoint(Checkpoint { cap: u32_(v[0]), block: u32_(v[1]), counts: counts(&v[2..6]) })
        }
        MarkerKind::End => {
            let v = fields(END_FIELDS, tag)?;
            Tag::End(EndTag {
                id: id(&v[0..6]),
                block: u32_(v[6]),
                counts: counts(&v[7..11]),
                hooks: u32_(v[11]),
                capped: v[12] == 1,
                other_ctx: u32_(v[13]),
                other_ctx_sat: v[14] == 1,
                other_thread: u32_(v[15]),
                other_thread_sat: v[16] == 1,
                immediate: u32_(v[17]),
                immediate_sat: v[18] == 1,
                ckpt_missing: u32_(v[19]),
                unsafe_draws: u32_(v[20]),
                no_space: u32_(v[21]),
                reentrant: u32_(v[22]),
                invalid: u32_(v[23]),
                mismatch: u32_(v[24]),
                counts_sat: v[25] == 1,
                hook_fault: v[26] == 1,
            })
        }
    })
}

/// Formats a tag in the v1 grammar (the reader's reference encoding; `parse_tag(format_tag(t)) == t` for every
/// tag whose values are in range).
pub fn format_tag(t: &Tag) -> String {
    let id = |i: &Identity| {
        format!("cap={} conn={} wire={} dom={} owner={} frame={}", i.cap, i.conn, i.wire, i.dom, i.owner, i.frame)
    };
    let counts = |r: &Receipts| format!("ev={} lost={} sup={} ref={}", r.ev, r.lost, r.sup, r.refused);
    let f = |b: bool| u8::from(b);
    match t {
        Tag::Begin(i) => format!("{TAG_VERSION} {}", id(i)),
        Tag::Checkpoint(c) => format!("{TAG_VERSION} cap={} block={} {}", c.cap, c.block, counts(&c.counts)),
        Tag::End(e) => format!(
            "{TAG_VERSION} {} block={} {} hooks={} capped={} other_ctx={} other_ctx_sat={} other_thread={} \
             other_thread_sat={} immediate={} immediate_sat={} ckpt_missing={} unsafe={} no_space={} reentrant={} \
             invalid={} mismatch={} counts_sat={} hook_fault={} coverage={COVERAGE}",
            id(&e.id),
            e.block,
            counts(&e.counts),
            e.hooks,
            f(e.capped),
            e.other_ctx,
            f(e.other_ctx_sat),
            e.other_thread,
            f(e.other_thread_sat),
            e.immediate,
            f(e.immediate_sat),
            e.ckpt_missing,
            e.unsafe_draws,
            e.no_space,
            e.reentrant,
            e.invalid,
            e.mismatch,
            f(e.counts_sat),
            f(e.hook_fault)
        ),
    }
}

/// The value rules a grammatical tag must also satisfy (its semantic validity): `cap` is not 0 (reserved for
/// refusals before a request is claimed); BEGIN and END name a real frame owner, not 0 (NONE: a capture is never
/// armed under the NONE token; frame 0 of a real owner is valid); a CHECKPOINT names block 0..=62; an END's block is
/// floor((hooks - 1) / 4096), or 0 with zero counts when hooks = 0.  A violation is an "inconsistent tag" anomaly on
/// the cap, which blocks FINALIZED (the marker is still placed by its sequence number).
pub fn consistency(t: &Tag) -> Result<(), String> {
    if t.cap() == 0 {
        return Err("cap 0 is reserved for refusals before a request is claimed".into());
    }
    let owner = match t {
        Tag::Begin(i) => Some(i.owner),
        Tag::End(e) => Some(e.id.owner),
        Tag::Checkpoint(_) => None,
    };
    if owner == Some(0) {
        return Err("owner 0 (NONE) is not a capture frame".into());
    }
    match t {
        Tag::Begin(_) => Ok(()),
        Tag::Checkpoint(c) if c.block >= MAX_BLOCKS - 1 => {
            Err(format!("checkpoint block {} (a capture has at most {MAX_BLOCKS} blocks)", c.block))
        }
        Tag::Checkpoint(_) => Ok(()),
        Tag::End(e) if e.hooks == 0 && (e.block != 0 || e.counts != Receipts::default()) => {
            Err(format!("hooks=0 with block={} and nonzero counts", e.block))
        }
        Tag::End(e) if e.hooks > 0 && e.block != (e.hooks - 1) / BLOCK_DRAWS => {
            Err(format!("block={} but hooks={} puts the last draw in block {}", e.block, e.hooks, (e.hooks - 1) / BLOCK_DRAWS))
        }
        Tag::End(_) => Ok(()),
    }
}

// ---- results ------------------------------------------------------------------------------------------------------

/// A marker as received: its event sequence number, its tag and the event's frame token (owner, frame id).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MarkerAt<T> {
    pub seq: u64,
    pub tag: T,
    pub token: (u32, u64),
}

/// The draws between two consecutive received markers of one capture (or after the last one when END is missing).
/// It is exactly one block when its markers agree and its counts prove every draw was received; only then are its
/// ordinals known.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    /// The marker that opens it (BEGIN or a CHECKPOINT) and the one that closes it (None: END missing).
    pub from_seq: u64,
    pub to_seq: Option<u64>,
    /// The block the opening marker announces (BEGIN: 0, CHECKPOINT b: b + 1).
    pub opened: u32,
    /// The block whose counts the closing marker carries, and those counts.
    pub closed: Option<(u32, Receipts)>,
    /// Its draws: `RsxCapture::draws[first_draw .. first_draw + draws]`.
    pub first_draw: u32,
    pub draws: u32,
    /// Draw-kind events in the segment and draw pairs lying wholly inside it.
    pub draw_events: u64,
    pub inside_pairs: u64,
    /// LOSS records overlapping the segment.
    pub loss_records: u32,
    /// Exact: `opened` is its block, ordinals are 4096 * opened + position.
    pub exact: bool,
}

/// Why a capture is not FINALIZED (each with its count).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Reason {
    /// END not received.  The reason is unknown from the file; `final_state` is the domain's FINAL state when the
    /// file has one, `superseded_by` a later BEGIN on the domain that arrived while this capture was open.
    MissingEnd { final_state: Option<u8>, superseded_by: Option<u32> },
    MissingBegin,
    LossInside { records: u32, events: u64 },
    /// Draw events in the capture without known pairing (unpaired, censored, open, or across a marker).
    UnpairedDraws(u64),
    /// Nonzero lost / suppressed / refused receipt counts summed over END and every checkpoint.
    ReceiptCounts { lost: u64, sup: u64, refused: u64 },
    PairsNotHooks { pairs: u64, hooks: u32 },
    Capped,
    Immediate { count: u32, sat: bool },
    /// `reported`: END's ckpt_missing; `absent`: checkpoints END implies that were not received.
    MissingCheckpoints { reported: u32, absent: u32 },
    /// The blocks (by the number their opening marker announced) whose ordinals are UNKNOWN.
    UnknownBlocks(Vec<u32>),
    Unsafe(u32),
    NoSpace(u32),
    Reentrant(u32),
    Invalid(u32),
    Mismatch(u32),
    CountsSat,
    HookFault,
    /// The BEGIN event's frame token differs from the tag's (owner, frame).
    FrameToken { owner: u32, frame: u64 },
    /// More than [`DRAW_CAP`] draw pairs: refused, no draw is listed.
    OverCap(u64),
    /// The file's stored-draw budget would be exceeded by this capture's `pairs`: refused, no draw is listed.
    DrawBudget { pairs: u64, limit: u64 },
    /// BEGIN and every checkpoint are missing: there is no interval start, so which draws belong to the capture is
    /// UNKNOWN (not zero).
    DrawsUnattributed,
    /// Anomalies attached to this capture.
    Anomalies(u32),
}

fn final_word(s: u8) -> &'static str {
    match s {
        STATE_FINALIZED => "FINALIZED",
        STATE_ABANDONED => "ABANDONED",
        STATE_OVERFLOWED => "OVERFLOWED",
        _ => "?",
    }
}

impl fmt::Display for Reason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Reason::MissingEnd { final_state, superseded_by } => {
                write!(f, "missing END, reason unknown")?;
                if let Some(c) = superseded_by {
                    write!(f, " (a BEGIN of cap {c} arrived while it was open)")?;
                }
                match final_state {
                    Some(s) => write!(f, " (domain FINAL: {})", final_word(*s)),
                    None => write!(f, " (no domain FINAL)"),
                }
            }
            Reason::MissingBegin => write!(f, "missing BEGIN"),
            Reason::LossInside { records, events } => write!(f, "LOSS inside: {records} range(s), {events} event(s)"),
            Reason::UnpairedDraws(n) => write!(f, "{n} draw event(s) without known pairing"),
            Reason::ReceiptCounts { lost, sup, refused } => {
                write!(f, "nonzero receipt counts: lost {lost}, suppressed {sup}, refused {refused}")
            }
            Reason::PairsNotHooks { pairs, hooks } => write!(f, "{pairs} draw pair(s) received, hooks={hooks}"),
            Reason::Capped => write!(f, "draw cap reached (capped=1): later draws were not instrumented"),
            Reason::Immediate { count, sat } => write!(
                f,
                "{count}{} immediate-mode draw(s) not instrumented",
                if *sat { "+ (saturated)" } else { "" }
            ),
            Reason::MissingCheckpoints { reported, absent } => {
                write!(f, "missing checkpoints: {reported} not registered, {absent} not received")
            }
            Reason::UnknownBlocks(b) => {
                let names: Vec<String> = b.iter().map(u32::to_string).collect();
                write!(f, "block{} {} UNKNOWN", if b.len() == 1 { "" } else { "s" }, names.join(", "))
            }
            Reason::Unsafe(n) => write!(f, "{n} draw(s) through Unsafe calls not instrumented"),
            Reason::NoSpace(n) => write!(f, "{n} draw(s) not instrumented (no space in the segment)"),
            Reason::Reentrant(n) => write!(f, "{n} reentrant draw(s) not instrumented"),
            Reason::Invalid(n) => write!(f, "{n} invalid draw call(s) not instrumented"),
            Reason::Mismatch(n) => write!(f, "{n} draw(s) emitted a different word count"),
            Reason::CountsSat => write!(f, "a side counter saturated (counts_sat=1)"),
            Reason::HookFault => write!(f, "hook fault (hook_fault=1)"),
            Reason::FrameToken { owner, frame } => {
                write!(f, "BEGIN event frame token (owner {owner}, frame {frame}) differs from the tag")
            }
            Reason::OverCap(n) => write!(f, "over cap: {n} draw pairs (more than {DRAW_CAP}); draws not listed"),
            Reason::DrawBudget { pairs, limit } => write!(
                f,
                "refused: {pairs} draw pairs would exceed the file's stored-draw budget of {limit}; draws not listed"
            ),
            Reason::DrawsUnattributed => write!(f, "draws UNKNOWN: BEGIN and every checkpoint missing, no interval start"),
            Reason::Anomalies(n) => write!(f, "{n} anomal{}", if *n == 1 { "y" } else { "ies" }),
        }
    }
}

/// The persisted completion state of one capture.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureState {
    Finalized,
    Partial,
    /// A duplicate cap, or a BEGIN that arrived while another capture of the domain was open.
    Anomaly,
}

impl CaptureState {
    pub fn word(self) -> &'static str {
        match self {
            CaptureState::Finalized => "FINALIZED",
            CaptureState::Partial => "PARTIAL",
            CaptureState::Anomaly => "ANOMALY",
        }
    }
}

/// One analysed capture.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RsxCapture {
    pub cap: u32,
    /// Index into `ext.sources` of the GPU source its markers were placed on (None: no marker placed).
    pub source: Option<usize>,
    pub begin: Option<MarkerAt<Identity>>,
    pub end: Option<MarkerAt<EndTag>>,
    pub checkpoints: Vec<MarkerAt<Checkpoint>>,
    /// Indices into the source's `raw.pairs`, in BEGIN sequence order; empty when refused over the cap.
    pub draws: Vec<u32>,
    /// Draw pairs attributed (also counted past the cap).
    pub draw_pairs: u64,
    /// No interval start (BEGIN and every checkpoint missing): the capture's draws are UNKNOWN, not zero.
    pub attribution_unknown: bool,
    pub segments: Vec<Segment>,
    pub loss_records: u32,
    pub loss_events: u64,
    pub state: CaptureState,
    /// Empty only when FINALIZED.
    pub reasons: Vec<Reason>,
    /// Anomalies attached to this cap.
    pub anomalies: u32,
}

impl RsxCapture {
    /// The ordinal of `draws[i]`, or None (UNKNOWN) when its block is not exact.
    pub fn ordinal(&self, i: usize) -> Option<u32> {
        let i = u32::try_from(i).ok()?;
        let at = self.segments.partition_point(|s| s.first_draw + s.draws <= i);
        let s = self.segments.get(at).filter(|s| s.first_draw <= i && s.exact)?;
        s.opened.checked_mul(BLOCK_DRAWS)?.checked_add(i - s.first_draw)
    }

    /// "FINALIZED", or the state word and every reason.
    pub fn state_line(&self) -> String {
        if self.reasons.is_empty() {
            return self.state.word().to_string();
        }
        let r: Vec<String> = self.reasons.iter().map(Reason::to_string).collect();
        format!("{}: {}", self.state.word(), r.join("; "))
    }

    /// The frame named by BEGIN (else END).
    pub fn frame(&self) -> Option<u64> {
        self.begin.map(|b| b.tag.frame).or(self.end.map(|e| e.tag.id.frame))
    }
}

/// What is wrong in the reserved-marker namespace.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AnomalyKind {
    /// A reserved name whose tag does not parse.
    UnparsableTag { name: String, detail: String },
    /// A `ps3dk.rsxcap.` tag on a descriptor that does not have a reserved name.
    TagUnderOtherName { name: String },
    /// A reserved (or `ps3dk.rsxcap.`-tagged) descriptor used by a zone BEGIN/END rather than a MARK.
    NotAMark { name: String },
    /// A parsable reserved MARK the extension reader did not accept (its depth contradicts the stack, or it follows
    /// a terminal PROTOCOL ERROR): not placed.
    UntrustedMarker { marker: MarkerKind },
    /// A draw-kind descriptor with a malformed or unknown-kind tag, or whose name and tag disagree.
    DrawTag { name: String, detail: String },
    /// A second BEGIN or END for a cap.
    DuplicateCap { marker: MarkerKind },
    /// A BEGIN while the domain already had capture `open` open (that one: PARTIAL, missing END).
    BeginWhileOpen { open: u32 },
    /// A CHECKPOINT or END with no open BEGIN for its cap on this domain.
    NoOpenBegin { marker: MarkerKind },
    /// A CHECKPOINT whose block is not the previous one + 1 (and no LOSS or reported missing checkpoint explains it).
    CheckpointOrder { expected: u32, got: u32 },
    /// The tag's `dom` is not the domain the marker was recorded on.
    WrongDomain { dom: u32 },
    /// END's identity fields differ from BEGIN's.
    IdentityMismatch,
    /// A grammatical tag breaking a value rule ([`consistency`]), or too many checkpoints.
    Inconsistent { detail: String },
    /// More than [`MAX_CAPTURES`] caps: only the first are analysed.
    TooManyCaptures { total: usize },
}

impl fmt::Display for AnomalyKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AnomalyKind::UnparsableTag { name, detail } => write!(f, "{name}: unparsable tag ({detail})"),
            AnomalyKind::TagUnderOtherName { name } => write!(f, "a {TAG_PREFIX} tag under the name {name:?}"),
            AnomalyKind::NotAMark { name } => write!(f, "{name:?} used by a zone event, not a MARK"),
            AnomalyKind::UntrustedMarker { marker } => {
                write!(f, "{} not accepted by the reader's pairing (bad depth or after a PROTOCOL ERROR)", marker.word())
            }
            AnomalyKind::DrawTag { name, detail } => write!(f, "draw descriptor {name:?}: {detail}"),
            AnomalyKind::DuplicateCap { marker } => write!(f, "duplicate cap (a second {})", marker.word()),
            AnomalyKind::BeginWhileOpen { open } => write!(f, "BEGIN while cap {open} was open on the domain"),
            AnomalyKind::NoOpenBegin { marker } => write!(f, "{} with no open BEGIN for its cap", marker.word()),
            AnomalyKind::CheckpointOrder { expected, got } => {
                write!(f, "checkpoint block {got}, expected {expected} (that block onward UNKNOWN)")
            }
            AnomalyKind::WrongDomain { dom } => write!(f, "tag names dom={dom}, recorded on another domain"),
            AnomalyKind::IdentityMismatch => write!(f, "END identity fields differ from BEGIN"),
            AnomalyKind::Inconsistent { detail } => write!(f, "inconsistent tag: {detail}"),
            AnomalyKind::TooManyCaptures { total } => {
                write!(f, "too many captures: {total} caps, only the first {MAX_CAPTURES} analysed")
            }
        }
    }
}

/// One anomaly: where (GPU source and event seq, when it is about one marker), which cap, and what.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Anomaly {
    pub source: Option<SourceKey>,
    pub seq: Option<u64>,
    pub cap: Option<u32>,
    pub kind: AnomalyKind,
}

impl fmt::Display for Anomaly {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(s) = self.source {
            write!(f, "{s}")?;
            if let Some(q) = self.seq {
                write!(f, " seq {q}")?;
            }
            write!(f, ": ")?;
        }
        if let Some(c) = self.cap {
            write!(f, "cap {c}: ")?;
        }
        write!(f, "{}", self.kind)
    }
}

/// Every capture in one file.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RsxCaptures {
    /// The analysed captures, by ascending cap (at most [`MAX_CAPTURES`]).
    pub captures: Vec<RsxCapture>,
    /// Caps beyond the analysis bound, listed by cap only (at most [`MAX_LISTED_CAPS`]).
    pub not_analysed: Vec<u32>,
    /// Further caps beyond the analysis bound, counted only.
    pub not_analysed_omitted: u64,
    pub anomalies: Vec<Anomaly>,
    /// Anomalies beyond [`MAX_ANOMALIES`] (counted, not kept).
    pub anomalies_omitted: u64,
    /// Draw indices stored over all captures (charged against [`Budgets::stored_draws`]).
    pub stored_draws: u64,
}

impl RsxCaptures {
    pub fn anomaly_count(&self) -> u64 {
        self.anomalies.len() as u64 + self.anomalies_omitted
    }
    pub fn get(&self, cap: u32) -> Option<&RsxCapture> {
        self.captures.iter().find(|c| c.cap == cap)
    }
}

// ---- the live request (typed input, never persisted) ------------------------------------------------------------

/// The live link's state for one request, as its ACKs reported it.  Reasons are the console's u32 codes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AckState {
    /// No request on this link (a file opened, or a cap of another connection).
    None,
    Accepted,
    /// The console enqueued the END (persistence is NOT certified by it).
    Done,
    Refused(u32),
    Abandoned(u32),
    /// The transport ended before the terminal ACK.
    Unknown,
}

impl AckState {
    /// The ACK record's (state, reason) as an AckState: ACCEPTED 1, DONE 2, REFUSED 3, ABANDONED 4; any other state
    /// is a protocol error (None).
    pub fn from_wire(state: u32, reason: u32) -> Option<AckState> {
        match state {
            1 => Some(AckState::Accepted),
            2 => Some(AckState::Done),
            3 => Some(AckState::Refused(reason)),
            4 => Some(AckState::Abandoned(reason)),
            _ => None,
        }
    }
}

/// What to show for a request: the data's state (or PENDING) next to the console's own report.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Shown {
    /// The persisted state from the data.
    Persisted(CaptureState, Vec<Reason>),
    /// ACCEPTED or DONE, the link is live, and END is not in the data yet.
    Pending,
    /// Nothing in the data, and nothing expected (no ACK, REFUSED, ABANDONED, or UNKNOWN).
    NoData,
}

/// A request as shown: `shown` comes from the data (PENDING only while the link is live), `console` is the ACK
/// state, always reported separately and never used as the file's reason.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RequestView {
    pub shown: Shown,
    pub console: AckState,
}

/// Combines the ACK state with what the data holds for the request's cap.  ACCEPTED or DONE with no END in the data
/// is PENDING while `link_live`; once the link ends it is the data's PARTIAL (missing END), or PARTIAL (missing
/// BEGIN, missing END) when no marker arrived at all.
pub fn request_view(ack: AckState, capture: Option<&RsxCapture>, link_live: bool) -> RequestView {
    let accepted = matches!(ack, AckState::Accepted | AckState::Done);
    let shown = match capture {
        Some(c) if c.end.is_some() => Shown::Persisted(c.state, c.reasons.clone()),
        Some(_) if accepted && link_live => Shown::Pending,
        Some(c) => Shown::Persisted(c.state, c.reasons.clone()),
        None if accepted && link_live => Shown::Pending,
        None if accepted => Shown::Persisted(
            CaptureState::Partial,
            vec![Reason::MissingBegin, Reason::MissingEnd { final_state: None, superseded_by: None }],
        ),
        None => Shown::NoData,
    };
    RequestView { shown, console: ack }
}

// ---- the analysis -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone)]
enum Class {
    Plain,
    Draw,
    DrawFault(String),
    Marker { kind: MarkerKind, tag: Result<Tag, String>, inconsistent: Option<String> },
    Foreign,
}

#[derive(Debug, Clone, Copy)]
enum Bound {
    Begin,
    Ckpt(u32, Receipts),
    End(u32, Receipts),
}

#[derive(Debug, Default)]
struct Build {
    source: Option<usize>,
    begin: Option<MarkerAt<Identity>>,
    end: Option<MarkerAt<EndTag>>,
    checkpoints: Vec<MarkerAt<Checkpoint>>,
    bounds: Vec<(u64, Bound)>,
    /// The interval end: END or a superseding marker; None while open (to the end of the source).
    hi: Option<u64>,
    superseded_by: Option<u32>,
    anomaly_state: bool,
    anomalies: u32,
    next_block: u32,
    /// Checkpoints that skipped forward: (bound index, expected, got), judged once END and LOSS are known.
    skips: Vec<(usize, u32, u32)>,
    /// The first segment made UNKNOWN by a backward checkpoint.
    backward_from: Option<usize>,
    headless: bool,
    draws: Vec<u32>,
    draw_pairs: u64,
    over_cap: bool,
    budget_refused: bool,
    segs: Vec<SegAcc>,
    loss_records: u32,
    loss_events: u64,
}

#[derive(Debug, Default, Clone, Copy)]
struct SegAcc {
    draws: u32,
    events: u64,
    inside: u64,
    loss: u32,
}

impl Build {
    fn lo(&self) -> Option<u64> {
        self.bounds.first().map(|b| b.0)
    }
    /// The segment holding `seq` (strictly after its opening marker and before the next one), or None.
    fn seg_at(&self, seq: u64) -> Option<usize> {
        let j = self.bounds.partition_point(|b| b.0 < seq).checked_sub(1)?;
        (!matches!(self.bounds[j].1, Bound::End(..))).then_some(j)
    }
    fn right_of(&self, j: usize) -> u64 {
        self.bounds.get(j + 1).map_or(self.hi.unwrap_or(u64::MAX), |x| x.0)
    }
}

/// The capture open on a domain while the markers are walked.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Open {
    /// An analysed capture (index into the capture records).
    Retained(usize),
    /// A cap beyond [`MAX_CAPTURES`]: validated, not analysed.
    Omitted(u32),
}

fn retained(o: Option<Open>) -> Option<usize> {
    match o {
        Some(Open::Retained(i)) => Some(i),
        _ => None,
    }
}

fn open_cap(o: Open, analysed: &[u32]) -> u32 {
    match o {
        Open::Retained(i) => analysed[i],
        Open::Omitted(c) => c,
    }
}

/// What is kept for a cap beyond the analysis bound.
#[derive(Debug, Default, Clone, Copy)]
struct Omitted {
    begin: Option<Identity>,
    ended: bool,
}

/// A capture interval on one source: (first marker seq, end seq or u64::MAX, capture index).
type Interval = (u64, u64, usize);

struct Ctx {
    anomalies: Vec<Anomaly>,
    omitted: u64,
}

impl Ctx {
    fn push(&mut self, a: Anomaly) {
        if self.anomalies.len() < MAX_ANOMALIES {
            self.anomalies.push(a);
        } else {
            self.omitted += 1;
        }
    }
}

fn classify(a: &Analysis) -> Vec<Class> {
    // Bounded by the reader: descriptor indices are below its max_descriptors (at most 65536).
    let n = a.capture.descriptors.keys().next_back().map_or(0, |&k| k as usize + 1);
    let mut v = vec![Class::Plain; n];
    for (&i, d) in &a.capture.descriptors {
        v[i as usize] = match descriptor_class(&d.name, &d.logical_tag) {
            DescClass::Marker(kind) => {
                let tag = parse_tag(kind, &d.logical_tag);
                let inconsistent = tag.as_ref().ok().and_then(|t| consistency(t).err());
                Class::Marker { kind, tag, inconsistent }
            }
            DescClass::Draw(_) => Class::Draw,
            DescClass::DrawFault(why) => Class::DrawFault(why),
            DescClass::ForeignTag => Class::Foreign,
            DescClass::Plain => Class::Plain,
        };
    }
    v
}

fn class_of(classes: &[Class], desc: u32) -> &Class {
    classes.get(desc as usize).unwrap_or(&Class::Plain)
}

/// The draw kind of a descriptor: Some only for a genuine draw-kind descriptor ([`descriptor_class`]).  The
/// classifier and the viewer both use this rule.
pub fn draw_kind(a: &Analysis, desc: u32) -> Option<DrawKind> {
    let d = a.capture.descriptors.get(&desc)?;
    match descriptor_class(&d.name, &d.logical_tag) {
        DescClass::Draw(k) => Some(k),
        _ => None,
    }
}

/// The event with sequence number `seq` in a source (events are in sequence order).
pub fn event_at(s: &SourceReport, seq: u64) -> Option<&RawEvent> {
    s.raw.events.binary_search_by_key(&seq, |e| e.seq).ok().map(|i| &s.raw.events[i])
}

/// Finds and classifies every RSX frame capture in `a` under the default [`Budgets`].
pub fn rsx_captures(a: &Analysis) -> RsxCaptures {
    rsx_captures_with(a, &Budgets::default())
}

/// Finds and classifies every RSX frame capture in `a` (see the module documentation).
pub fn rsx_captures_with(a: &Analysis, budgets: &Budgets) -> RsxCaptures {
    let Some(x) = &a.capture.ext else { return RsxCaptures::default() };
    let classes = classify(a);
    let name = |desc: u32| a.capture.descriptors.get(&desc).map(|d| d.name.clone()).unwrap_or_default();
    let gpu: Vec<usize> =
        x.sources.iter().enumerate().filter(|(_, s)| matches!(s.key, SourceKey::Gpu(_))).map(|(i, _)| i).collect();

    // Pass 1: the caps named by parsable reserved MARK events (trusted or not: an untrusted marker still names its
    // cap, so the cap is listed with that fault rather than vanishing).
    let mut caps = BTreeSet::new();
    for &si in &gpu {
        for e in &x.sources[si].raw.events {
            if let (EV_MARK, Class::Marker { tag: Ok(t), .. }) = (e.kind, class_of(&classes, e.desc)) {
                caps.insert(t.cap());
            }
        }
    }
    let mut ctx = Ctx { anomalies: Vec::new(), omitted: 0 };
    let analysed: Vec<u32> = caps.iter().copied().take(MAX_CAPTURES).collect();
    let extra_caps = caps.len().saturating_sub(MAX_CAPTURES);
    let not_analysed: Vec<u32> = caps.iter().copied().skip(MAX_CAPTURES).take(MAX_LISTED_CAPS).collect();
    if extra_caps > 0 {
        ctx.push(Anomaly { source: None, seq: None, cap: None, kind: AnomalyKind::TooManyCaptures { total: caps.len() } });
    }
    let index: BTreeMap<u32, usize> = analysed.iter().enumerate().map(|(i, &c)| (c, i)).collect();
    let mut b: Vec<Build> = analysed.iter().map(|_| Build::default()).collect();
    // Caps beyond the analysis bound: their BEGIN identity and whether END was seen (bounded by the cap count,
    // itself bounded by the descriptor count), so their markers are validated like any other.
    let mut omitted: BTreeMap<u32, Omitted> = BTreeMap::new();

    // Pass 2: the markers, per GPU source in sequence order.  Only markers the extension reader ACCEPTED (its
    // `raw.marks`) are placed; a reserved MARK it did not accept (a depth that contradicts its stack, or any MARK
    // after a terminal PROTOCOL ERROR) is an untrusted marker.  Every namespace fault taints the named cap and the
    // capture open on that domain at that sequence number.
    for &si in &gpu {
        let s = &x.sources[si];
        let key = s.key;
        // The capture open on this domain: a retained one, or one beyond the analysis bound (validated, not analysed).
        let mut open: Option<Open> = None;
        let mut loss_pending = false;
        let (mut li, mut mi) = (0usize, 0usize);
        for e in &s.raw.events {
            // A LOSS since the last placed marker may hold an ABSENT marker: it explains a missing END (a later BEGIN
            // while open) or a missing BEGIN (a CHECKPOINT or END of an unseen cap), never a received contradiction.
            while li < s.raw.losses.len() && s.raw.losses[li].first < e.seq {
                loss_pending = true;
                li += 1;
            }
            while mi < s.raw.marks.len() && s.raw.marks[mi] < e.seq {
                mi += 1;
            }
            let trusted = s.raw.marks.get(mi) == Some(&e.seq);
            let class = class_of(&classes, e.desc);
            let fault = match class {
                Class::Plain | Class::Draw => None,
                Class::DrawFault(detail) => Some((None, AnomalyKind::DrawTag { name: name(e.desc), detail: detail.clone() })),
                Class::Foreign if e.kind == EV_MARK => Some((None, AnomalyKind::TagUnderOtherName { name: name(e.desc) })),
                Class::Foreign => Some((None, AnomalyKind::NotAMark { name: name(e.desc) })),
                Class::Marker { tag, .. } if e.kind != EV_MARK => {
                    Some((tag.as_ref().ok().map(Tag::cap), AnomalyKind::NotAMark { name: name(e.desc) }))
                }
                Class::Marker { kind, tag: Err(detail), .. } => {
                    Some((None, AnomalyKind::UnparsableTag { name: kind.name().into(), detail: detail.clone() }))
                }
                Class::Marker { kind, tag: Ok(t), .. } if !trusted => {
                    Some((Some(t.cap()), AnomalyKind::UntrustedMarker { marker: *kind }))
                }
                Class::Marker { .. } => None,
            };
            // Taints the named cap (when retained) and the retained capture open here (when it is another one).
            let flag = |ctx: &mut Ctx, b: &mut [Build], open: Option<Open>, cap: Option<u32>, kind: AnomalyKind| {
                let named = cap.and_then(|c| index.get(&c).copied());
                if let Some(ci) = named {
                    b[ci].anomalies += 1;
                }
                let open_r = retained(open);
                if let Some(oi) = open_r.filter(|&oi| Some(oi) != named) {
                    b[oi].anomalies += 1;
                }
                let shown = cap.or(open_r.map(|oi| analysed[oi]));
                ctx.push(Anomaly { source: Some(key), seq: Some(e.seq), cap: shown, kind });
            };
            // Closes the retained capture open here at this marker (a later marker supersedes it).
            let supersede = |b: &mut [Build], open: Option<Open>, by: u32| {
                if let Some(oi) = retained(open) {
                    b[oi].hi = Some(e.seq);
                    b[oi].superseded_by = Some(by);
                }
            };
            if let Some((cap, kind)) = fault {
                flag(&mut ctx, &mut b, open, cap, kind);
                continue;
            }
            let Class::Marker { tag: Ok(t), inconsistent, .. } = class else { continue };
            let t = *t;
            let cap = t.cap();
            let c_ = Some(cap);
            if let Some(detail) = inconsistent {
                flag(&mut ctx, &mut b, open, c_, AnomalyKind::Inconsistent { detail: detail.clone() });
            }
            if let Some(d) = t.dom() {
                if d != s.domain_id {
                    flag(&mut ctx, &mut b, open, c_, AnomalyKind::WrongDomain { dom: d });
                }
            }
            let Some(&ci) = index.get(&cap) else {
                // A cap beyond the analysis bound: the same boundary and identity validation, no record, no draws.
                // It opens and closes an interval on this domain, so a retained capture it nests in, supersedes or
                // strays into is tainted exactly as by a retained cap.
                let om = omitted.entry(cap).or_default();
                let (begun, ended) = (om.begin, om.ended);
                let is_open = open == Some(Open::Omitted(cap));
                let headless = !is_open && loss_pending && begun.is_none() && !ended;
                match t {
                    Tag::Begin(id) => {
                        if begun.is_some() || ended {
                            flag(&mut ctx, &mut b, open, c_, AnomalyKind::DuplicateCap { marker: MarkerKind::Begin });
                            continue;
                        }
                        let was = open.take();
                        supersede(&mut b, was, cap);
                        if let (Some(o), false) = (was, loss_pending) {
                            flag(&mut ctx, &mut b, None, c_, AnomalyKind::BeginWhileOpen { open: open_cap(o, &analysed) });
                        }
                        omitted.get_mut(&cap).expect("entry").begin = Some(id);
                        open = Some(Open::Omitted(cap));
                    }
                    Tag::Checkpoint(_) => {
                        if !is_open && !headless {
                            flag(&mut ctx, &mut b, open, c_, AnomalyKind::NoOpenBegin { marker: MarkerKind::Checkpoint });
                            continue;
                        }
                        if headless {
                            supersede(&mut b, open, cap);
                            open = Some(Open::Omitted(cap));
                        }
                    }
                    Tag::End(end) => {
                        if ended {
                            flag(&mut ctx, &mut b, open, c_, AnomalyKind::DuplicateCap { marker: MarkerKind::End });
                            continue;
                        }
                        if !is_open && !headless {
                            flag(&mut ctx, &mut b, open, c_, AnomalyKind::NoOpenBegin { marker: MarkerKind::End });
                            continue;
                        }
                        if begun.is_some_and(|id| id != end.id) {
                            flag(&mut ctx, &mut b, None, c_, AnomalyKind::IdentityMismatch);
                        }
                        if headless {
                            supersede(&mut b, open, cap);
                        }
                        omitted.get_mut(&cap).expect("entry").ended = true;
                        open = None;
                    }
                }
                loss_pending = false;
                continue;
            };
            let token = (e.frame_owner, e.frame_id);
            let is_open = open == Some(Open::Retained(ci));
            match t {
                Tag::Begin(id) => {
                    if b[ci].begin.is_some() || b[ci].end.is_some() || !b[ci].bounds.is_empty() {
                        b[ci].anomaly_state = true;
                        flag(&mut ctx, &mut b, open, c_, AnomalyKind::DuplicateCap { marker: MarkerKind::Begin });
                        continue;
                    }
                    let was = open.take();
                    supersede(&mut b, was, cap);
                    if let (Some(o), false) = (was, loss_pending) {
                        b[ci].anomaly_state = true;
                        flag(&mut ctx, &mut b, None, c_, AnomalyKind::BeginWhileOpen { open: open_cap(o, &analysed) });
                    }
                    let c = &mut b[ci];
                    c.source = Some(si);
                    c.begin = Some(MarkerAt { seq: e.seq, tag: id, token });
                    c.bounds.push((e.seq, Bound::Begin));
                    open = Some(Open::Retained(ci));
                    loss_pending = false;
                }
                Tag::Checkpoint(k) => {
                    let headless = !is_open && loss_pending && b[ci].bounds.is_empty() && b[ci].end.is_none();
                    if !is_open && !headless {
                        flag(&mut ctx, &mut b, open, c_, AnomalyKind::NoOpenBegin { marker: MarkerKind::Checkpoint });
                        continue;
                    }
                    if b[ci].checkpoints.len() >= MAX_CHECKPOINTS {
                        let detail = format!("more than {MAX_CHECKPOINTS} checkpoints");
                        flag(&mut ctx, &mut b, open, c_, AnomalyKind::Inconsistent { detail });
                        continue;
                    }
                    if headless {
                        supersede(&mut b, open, cap);
                        open = Some(Open::Retained(ci));
                    }
                    let c = &mut b[ci];
                    if headless {
                        c.headless = true;
                        c.source = Some(si);
                        c.next_block = k.block;
                    } else if k.block > c.next_block {
                        c.skips.push((c.bounds.len(), c.next_block, k.block));
                    } else if k.block < c.next_block {
                        // The segment this checkpoint closes, and every later one, are UNKNOWN.
                        let from = c.bounds.len() - 1;
                        c.backward_from = Some(c.backward_from.map_or(from, |f| f.min(from)));
                        let exp = c.next_block;
                        flag(&mut ctx, &mut b, None, c_, AnomalyKind::CheckpointOrder { expected: exp, got: k.block });
                    }
                    let c = &mut b[ci];
                    c.next_block = c.next_block.max(k.block.saturating_add(1));
                    c.checkpoints.push(MarkerAt { seq: e.seq, tag: k, token });
                    c.bounds.push((e.seq, Bound::Ckpt(k.block, k.counts)));
                    loss_pending = false;
                }
                Tag::End(end) => {
                    let headless = !is_open && loss_pending && b[ci].bounds.is_empty() && b[ci].end.is_none();
                    if b[ci].end.is_some() {
                        b[ci].anomaly_state = true;
                        flag(&mut ctx, &mut b, open, c_, AnomalyKind::DuplicateCap { marker: MarkerKind::End });
                        continue;
                    }
                    if !is_open && !headless {
                        flag(&mut ctx, &mut b, open, c_, AnomalyKind::NoOpenBegin { marker: MarkerKind::End });
                        continue;
                    }
                    if let Some(bg) = b[ci].begin {
                        if bg.tag != end.id {
                            flag(&mut ctx, &mut b, None, c_, AnomalyKind::IdentityMismatch);
                        }
                    }
                    if headless {
                        supersede(&mut b, open, cap);
                    }
                    let c = &mut b[ci];
                    if headless {
                        c.headless = true;
                        c.source = Some(si);
                    }
                    c.end = Some(MarkerAt { seq: e.seq, tag: end, token });
                    c.bounds.push((e.seq, Bound::End(end.block, end.counts)));
                    c.hi = Some(e.seq);
                    open = None;
                    loss_pending = false;
                }
            }
        }
    }

    // Pass 3, per source, against the capture intervals (at most MAX_CAPTURES, disjoint, sorted by start).
    // 3a counts in-interval draw pairs, draw events and LOSS without allocating; the draw indices are then charged to
    // the per-capture cap and the file's `stored_draws` budget BEFORE any is stored; 3b stores only charged captures.
    let mut stored: u64 = 0;
    let mut per_source: Vec<(usize, Vec<Interval>)> = Vec::new();
    for &si in &gpu {
        let mut iv: Vec<Interval> = (0..b.len())
            .filter(|&i| b[i].source == Some(si))
            .filter_map(|i| b[i].lo().map(|lo| (lo, b[i].hi.unwrap_or(u64::MAX), i)))
            .collect();
        if iv.is_empty() {
            continue;
        }
        iv.sort_unstable();
        for &(_, _, i) in &iv {
            let n = b[i].bounds.len();
            b[i].segs = vec![SegAcc::default(); n];
        }
        per_source.push((si, iv));
    }
    let find = |iv: &[Interval], seq: u64| -> Option<usize> {
        let k = iv.partition_point(|x| x.0 < seq).checked_sub(1)?;
        (seq < iv[k].1).then_some(iv[k].2)
    };
    let is_draw = |desc: u32| matches!(class_of(&classes, desc), Class::Draw);
    for (si, iv) in &per_source {
        let s = &x.sources[*si];
        for p in s.raw.pairs.iter().filter(|p| is_draw(p.desc)) {
            let Some(ci) = find(iv, p.begin) else { continue };
            let c = &mut b[ci];
            let Some(j) = c.seg_at(p.begin) else { continue };
            c.draw_pairs += 1;
            c.segs[j].draws = c.segs[j].draws.saturating_add(1);
            if p.end < c.right_of(j) {
                c.segs[j].inside += 1;
            }
        }
        for e in s.raw.events.iter().filter(|e| is_draw(e.desc)) {
            let Some(ci) = find(iv, e.seq) else { continue };
            let c = &mut b[ci];
            if let Some(j) = c.seg_at(e.seq) {
                c.segs[j].events += 1;
            }
        }
        for l in &s.raw.losses {
            let last = l.first.saturating_add(l.count - 1);
            let mut k = iv.partition_point(|x| x.1 <= l.first);
            while let Some(&(lo, hi, ci)) = iv.get(k) {
                if lo >= last {
                    break;
                }
                let c = &mut b[ci];
                c.loss_records += 1;
                c.loss_events += last.min(hi.saturating_sub(1)).saturating_sub(l.first.max(lo + 1)) + 1;
                for j in 0..c.bounds.len() {
                    if matches!(c.bounds[j].1, Bound::End(..)) {
                        break;
                    }
                    if l.first < c.right_of(j) && last > c.bounds[j].0 {
                        c.segs[j].loss += 1;
                    }
                }
                k += 1;
            }
        }
    }
    // Charge, in ascending cap order.
    for c in b.iter_mut().filter(|c| c.draw_pairs > 0) {
        if c.draw_pairs > u64::from(DRAW_CAP) {
            c.over_cap = true;
        } else if stored + c.draw_pairs > budgets.stored_draws {
            c.budget_refused = true;
        } else {
            stored += c.draw_pairs;
            c.draws = Vec::with_capacity(c.draw_pairs as usize);
        }
    }
    for (si, iv) in &per_source {
        let s = &x.sources[*si];
        for (pi, p) in s.raw.pairs.iter().enumerate().filter(|(_, p)| is_draw(p.desc)) {
            let Some(ci) = find(iv, p.begin) else { continue };
            let c = &mut b[ci];
            if c.over_cap || c.budget_refused || c.seg_at(p.begin).is_none() {
                continue;
            }
            c.draws.push(pi as u32);
        }
        for &(_, _, ci) in iv {
            let pairs = &s.raw.pairs;
            let d = &mut b[ci].draws;
            if !d.windows(2).all(|w| pairs[w[0] as usize].begin < pairs[w[1] as usize].begin) {
                d.sort_unstable_by_key(|&i| pairs[i as usize].begin); // in place: no scratch
            }
        }
    }

    // Evaluation.
    let mut out = Vec::with_capacity(b.len());
    for (ci, c) in b.into_iter().enumerate() {
        let cap = analysed[ci];
        let mut anomalies = c.anomalies;
        let reported_missing = c.end.map_or(0, |e| e.tag.ckpt_missing);
        // Forward checkpoint skips.  A LOSS in the merged segment explains its absent checkpoint (local recovery);
        // so does END's ckpt_missing, when it covers every block skipped without a LOSS.  Any other skip is a
        // discontinuity: an anomaly, and the merged segment and every later one are UNKNOWN.
        let mut broken_from = c.backward_from;
        let unexplained: Vec<(usize, u32, u32)> = c
            .skips
            .iter()
            .copied()
            .filter(|&(bi, _, _)| !c.backward_from.is_some_and(|f| bi > f))
            .filter(|&(bi, _, _)| c.segs.get(bi.wrapping_sub(1)).is_none_or(|s| s.loss == 0))
            .collect();
        let skipped: u64 = unexplained.iter().map(|&(_, e, g)| u64::from(g - e)).sum();
        if skipped > u64::from(reported_missing) {
            for &(bi, expected, got) in &unexplained {
                anomalies += 1;
                broken_from = Some(broken_from.map_or(bi - 1, |f| f.min(bi - 1)));
                ctx.push(Anomaly {
                    source: c.source.map(|si| x.sources[si].key),
                    seq: Some(c.bounds[bi].0),
                    cap: Some(cap),
                    kind: AnomalyKind::CheckpointOrder { expected, got },
                });
            }
        }
        let stored_ok = !c.over_cap && !c.budget_refused;
        let mut segments = Vec::new();
        let mut pos = 0u32;
        for (j, &(from, bound)) in c.bounds.iter().enumerate() {
            let opened = match bound {
                Bound::Begin => 0,
                Bound::Ckpt(blk, _) => blk.saturating_add(1),
                Bound::End(..) => break,
            };
            let closed = match c.bounds.get(j + 1).map(|x| x.1) {
                Some(Bound::Ckpt(blk, r) | Bound::End(blk, r)) => Some((blk, r)),
                _ => None,
            };
            let acc = c.segs.get(j).copied().unwrap_or_default();
            let is_last = matches!(c.bounds.get(j + 1).map(|x| x.1), Some(Bound::End(..)));
            let exact = stored_ok
                && opened < MAX_BLOCKS
                && broken_from.is_none_or(|f| j < f)
                && acc.loss == 0
                && acc.events == 2 * acc.inside
                && u64::from(acc.draws) == acc.inside
                && closed.is_some_and(|(blk, r)| {
                    blk == opened && r.clean() && u64::from(r.ev) == 2 * acc.inside && (is_last || acc.draws == BLOCK_DRAWS)
                });
            segments.push(Segment {
                from_seq: from,
                to_seq: c.bounds.get(j + 1).map(|x| x.0),
                opened,
                closed,
                first_draw: pos,
                draws: acc.draws,
                draw_events: acc.events,
                inside_pairs: acc.inside,
                loss_records: acc.loss,
                exact,
            });
            pos = pos.saturating_add(acc.draws);
        }
        // With BEGIN and every checkpoint missing there is no interval start: the draws cannot be attributed.
        let attribution_unknown = c.begin.is_none() && segments.is_empty();

        let mut reasons = Vec::new();
        if c.over_cap {
            reasons.push(Reason::OverCap(c.draw_pairs));
        }
        if c.budget_refused {
            reasons.push(Reason::DrawBudget { pairs: c.draw_pairs, limit: budgets.stored_draws });
        }
        if c.begin.is_none() {
            reasons.push(Reason::MissingBegin);
        }
        if attribution_unknown {
            reasons.push(Reason::DrawsUnattributed);
        }
        if c.end.is_none() {
            let final_state = c.source.and_then(|si| x.sources[si].final_state);
            reasons.push(Reason::MissingEnd { final_state, superseded_by: c.superseded_by });
        }
        if let Some(bg) = c.begin {
            if bg.token != (bg.tag.owner, bg.tag.frame) {
                reasons.push(Reason::FrameToken { owner: bg.token.0, frame: bg.token.1 });
            }
        }
        if c.loss_records > 0 {
            reasons.push(Reason::LossInside { records: c.loss_records, events: c.loss_events });
        }
        let events: u64 = segments.iter().map(|s| s.draw_events).sum();
        let inside: u64 = segments.iter().map(|s| s.inside_pairs).sum();
        let unpaired = events.saturating_sub(2 * inside);
        if unpaired > 0 {
            reasons.push(Reason::UnpairedDraws(unpaired));
        }
        let mut r = (0u64, 0u64, 0u64);
        for k in c.checkpoints.iter().map(|k| k.tag.counts).chain(c.end.map(|e| e.tag.counts)) {
            r.0 += u64::from(k.lost);
            r.1 += u64::from(k.sup);
            r.2 += u64::from(k.refused);
        }
        if r != (0, 0, 0) {
            reasons.push(Reason::ReceiptCounts { lost: r.0, sup: r.1, refused: r.2 });
        }
        if let Some(e) = c.end.map(|e| e.tag) {
            if !attribution_unknown && c.draw_pairs != u64::from(e.hooks) {
                reasons.push(Reason::PairsNotHooks { pairs: c.draw_pairs, hooks: e.hooks });
            }
            if e.capped {
                reasons.push(Reason::Capped);
            }
            if e.immediate > 0 || e.immediate_sat {
                reasons.push(Reason::Immediate { count: e.immediate, sat: e.immediate_sat });
            }
            let absent = if c.headless { 0 } else { e.block.saturating_sub(c.checkpoints.len() as u32) };
            if e.ckpt_missing > 0 || absent > 0 {
                reasons.push(Reason::MissingCheckpoints { reported: e.ckpt_missing, absent });
            }
            for (n, mk) in [
                (e.unsafe_draws, Reason::Unsafe as fn(u32) -> Reason),
                (e.no_space, Reason::NoSpace),
                (e.reentrant, Reason::Reentrant),
                (e.invalid, Reason::Invalid),
                (e.mismatch, Reason::Mismatch),
            ] {
                if n > 0 {
                    reasons.push(mk(n));
                }
            }
            if e.counts_sat {
                reasons.push(Reason::CountsSat);
            }
            if e.hook_fault {
                reasons.push(Reason::HookFault);
            }
        }
        let unknown: Vec<u32> = segments
            .iter()
            .filter(|s| !s.exact && (s.closed.is_some() || s.draw_events > 0 || s.loss_records > 0))
            .map(|s| s.opened)
            .collect();
        if !unknown.is_empty() {
            reasons.push(Reason::UnknownBlocks(unknown));
        }
        if anomalies > 0 {
            reasons.push(Reason::Anomalies(anomalies));
        }
        let state = if c.anomaly_state {
            CaptureState::Anomaly
        } else if reasons.is_empty() {
            CaptureState::Finalized
        } else {
            CaptureState::Partial
        };
        out.push(RsxCapture {
            cap,
            source: c.source,
            begin: c.begin,
            end: c.end,
            checkpoints: c.checkpoints,
            draws: c.draws,
            draw_pairs: c.draw_pairs,
            attribution_unknown,
            segments,
            loss_records: c.loss_records,
            loss_events: c.loss_events,
            state,
            reasons,
            anomalies,
        });
    }
    RsxCaptures {
        captures: out,
        not_analysed,
        not_analysed_omitted: (extra_caps - extra_caps.min(MAX_LISTED_CAPS)) as u64,
        anomalies: ctx.anomalies,
        anomalies_omitted: ctx.omitted,
        stored_draws: stored,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maximal_tags_have_the_documented_lengths() {
        let id = Identity { cap: u32::MAX, conn: u32::MAX, wire: u32::MAX, dom: u32::MAX, owner: u32::MAX, frame: u64::MAX };
        let r = Receipts { ev: u32::MAX, lost: u32::MAX, sup: u32::MAX, refused: u32::MAX };
        assert_eq!(format_tag(&Tag::Begin(id)).len(), MAX_BEGIN_LEN);
        assert_eq!(format_tag(&Tag::Checkpoint(Checkpoint { cap: u32::MAX, block: u32::MAX, counts: r })).len(), MAX_CKPT_LEN);
        let e = EndTag {
            id,
            block: u32::MAX,
            counts: r,
            hooks: SIDE_MAX,
            capped: true,
            other_ctx: u32::MAX,
            other_ctx_sat: true,
            other_thread: u32::MAX,
            other_thread_sat: true,
            immediate: u32::MAX,
            immediate_sat: true,
            ckpt_missing: u32::MAX,
            unsafe_draws: SIDE_MAX,
            no_space: SIDE_MAX,
            reentrant: SIDE_MAX,
            invalid: SIDE_MAX,
            mismatch: SIDE_MAX,
            counts_sat: true,
            hook_fault: true,
        };
        let s = format_tag(&Tag::End(e));
        assert_eq!(s.len(), MAX_END_LEN);
        assert_eq!(parse_tag(MarkerKind::End, &s), Ok(Tag::End(e)));
    }
}

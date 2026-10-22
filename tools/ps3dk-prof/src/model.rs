//! Format constants, the decoded capture, and the verdict types.
//!
//! Every MALFORMED rule of the format has its own [`Rule`] variant, so a test (or a user) can tell exactly which rule
//! a file broke.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

pub const MAGIC: &[u8; 8] = b"PS3DKPRF";
pub const FILE_HEADER_LEN: usize = 16;
pub const CHUNK_HEADER_LEN: usize = 16;
pub const CHUNK_CRC_LEN: usize = 4;
pub const MAX_CHUNK_LEN: u32 = 1_048_576;
pub const MAX_STRING_LEN: usize = 512;
pub const MAX_THREADS: u32 = 1024;
pub const MAX_DESCRIPTORS: u32 = 65_536;
/// 2^63: sequences are below it, and it is the inclusive representation ceiling of the exact cumulative counters.
pub const SEQ_LIMIT: u64 = 1 << 63;
/// The most spans that can be open on one stack: depths 0..=255 (section 5.2).
pub const MAX_OPEN_SPANS: u64 = 256;
/// The UNKNOWN count of a NOT_QUIESCED final entry (section 6).
pub const UNKNOWN: u64 = u64::MAX;
pub const RECORD_LEN: usize = 32;
pub const FINAL_ENTRY_LEN: usize = 56;

pub const CHUNK_HEADER: u32 = 1;
pub const CHUNK_CLOCKS: u32 = 2;
pub const CHUNK_DESCRIPTORS: u32 = 3;
pub const CHUNK_THREADS: u32 = 4;
pub const CHUNK_RECORDS: u32 = 5;
pub const CHUNK_TRAILER: u32 = 6;
pub const CHUNK_RETIRED: u32 = 7;

pub const KIND_BEGIN: u8 = 1;
pub const KIND_BEGIN_PARENT: u8 = 2;
pub const KIND_END: u8 = 3;
pub const KIND_FRAME: u8 = 4;
pub const KIND_GAP: u8 = 5;
pub const KIND_REGERR: u8 = 6;

pub const FLAG_TOKEN: u16 = 1;
pub const FLAG_TRUNCATED_AT_STOP: u16 = 1;

pub const STATE_RETIRED: u8 = 1;
pub const STATE_ACKED: u8 = 2;
pub const STATE_NOT_QUIESCED: u8 = 3;

pub const LIMIT_SEQ_LIMIT: u8 = 1;
pub const LIMIT_COUNTS_UNKNOWN: u8 = 2;

pub const END_NOT_QUIESCED: u32 = 1;
pub const END_SEQ_LIMIT: u32 = 2;
pub const END_SINK_ERROR: u32 = 4;

/// One MALFORMED rule.  The section of the format that states it is in the comment.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Rule {
    // 1. Primitives
    StringTooLong,
    StringInvalidUtf8,
    StringContainsNul,
    // 2. File structure
    MagicMismatch,
    MajorVersion,
    FileFlagsNonzero,
    ChunkLenOverCap,
    ChunkSeqOrder,
    ChunkCrc,
    UnknownChunkType,
    HeaderNotChunk0,
    ClocksNotChunk1,
    DuplicateHeaderOrClocks,
    TrailingBytesAfterTrailer,
    SecondTrailer,
    // 3. Chunk payloads
    PayloadLength,
    HeaderAbi,
    HeaderConsumerMode,
    HeaderReserved,
    HeaderMaxThreads,
    HeaderMaxDescriptors,
    ClocksCount,
    ClocksDomain,
    ClocksReserved,
    ClocksFreqZero,
    DescriptorsCountZero,
    DescriptorScope,
    DescriptorReserved,
    DescriptorNameEmpty,
    DescriptorIndexRange,
    DescriptorIndexDuplicate,
    DescriptorIdMismatch,
    DescriptorIdDuplicate,
    ThreadsCountZero,
    ThreadIncarnationZero,
    ThreadSlotRange,
    IncarnationDuplicate,
    TooManyLiveIncarnations,
    /// A THREADS entry names a slot that a live (declared, not yet RETIRED) incarnation still holds.
    SlotAlreadyLive,
    RecordsCountZero,
    RecordsLength,
    RetiredCountZero,
    RetiredState,
    UndeclaredIncarnation,
    UndeclaredDescriptor,
    UndeclaredTokenOwner,
    RecordsAfterRetired,
    DuplicateFinalEntry,
    // 4. Records
    UnknownRecordKind,
    RecordReserved,
    DepthOnNonSpanRecord,
    TokenFlagMismatch,
    RegerrCode,
    SpanIdZero,
    BeginParentPlacement,
    SeqRange,
    SeqOrder,
    GapPlacement,
    GapSeqField,
    SpanIdDuplicate,
    DepthParentZeroMismatch,
    StackBeginMismatch,
    StackEndMismatch,
    KnownEmptyBeginDepth,
    KnownEmptyEnd,
    FrameIdSequence,
    /// A FRAME id above its own seq: ids 0..id were each assigned with a distinct earlier sequence (sections 5.1, 5.6).
    FrameIdBound,
    // 6. TRAILER and final entries
    TrailerEndFlagsReserved,
    TrailerReserved,
    FinalEntryState,
    FinalEntryReserved,
    FinalEntryLimitFlagsReserved,
    ExactEntryUnknown,
    State3Fields,
    /// An exact cumulative counter (written, dropped, refused, regerr) above its 2^63 representation ceiling (section 6).
    ExactCountRange,
    /// open_spans, an active-stack snapshot, above 256: depth is capped at 255 (sections 5.2 and 6).
    OpenSpansRange,
    WrittenCountMismatch,
    ProducedCountRange,
    ProducedCountArithmetic,
    DroppedCountArithmetic,
    /// regerr_count below the REGERR records written for the incarnation (section 5.3: the counter is incremented
    /// unconditionally, the record is best effort).
    RegerrCountBelowRecords,
    DescriptorCountMismatch,
    EndFlagNotQuiescedMismatch,
    EndFlagSeqLimitMismatch,
    EndFlagSinkErrorMismatch,
    // 9. Token resolution, rule 4
    TokenImpossible,
}

/// A MALFORMED verdict: the rule broken and where.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Malformed {
    pub rule: Rule,
    pub detail: String,
}

impl fmt::Display for Malformed {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "MALFORMED ({:?}): {}", self.rule, self.detail)
    }
}

pub(crate) fn malformed<T>(rule: Rule, detail: impl Into<String>) -> Result<T, Malformed> {
    Err(Malformed { rule, detail: detail.into() })
}

/// One reason a structurally sound capture is INCOMPLETE (section 9).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Incomplete {
    NoTrailer,
    TornTail,
    MissingFinalEntry(u32),
    NotQuiesced,
    SeqLimit,
    SinkError,
}

impl fmt::Display for Incomplete {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Incomplete::NoTrailer => write!(f, "the TRAILER is missing"),
            Incomplete::TornTail => write!(f, "the tail is torn"),
            Incomplete::MissingFinalEntry(inc) => write!(f, "incarnation {inc} has no final entry"),
            Incomplete::NotQuiesced => write!(f, "end_flags NOT_QUIESCED"),
            Incomplete::SeqLimit => write!(f, "end_flags SEQ_LIMIT"),
            Incomplete::SinkError => write!(f, "end_flags SINK_ERROR_BEFORE_TRAILER"),
        }
    }
}

/// The verdict of a capture that is not MALFORMED.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// TRANSPORT COMPLETE.
    Complete,
    Incomplete(Vec<Incomplete>),
}

/// A metric that may be unavailable.  Refused and unknown values are never shown as a number.
#[derive(Debug, Clone, PartialEq)]
pub enum Metric<T> {
    Value(T),
    Unknown(String),
    Refused(String),
}

impl<T> Metric<T> {
    pub fn value(&self) -> Option<&T> {
        match self {
            Metric::Value(v) => Some(v),
            _ => None,
        }
    }
    pub fn is_refused(&self) -> bool {
        matches!(self, Metric::Refused(_))
    }
}

impl<T: fmt::Display> fmt::Display for Metric<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Metric::Value(v) => write!(f, "{v}"),
            Metric::Unknown(why) => write!(f, "unknown ({why})"),
            Metric::Refused(why) => write!(f, "refused ({why})"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    pub minor: u16,
    pub app: String,
    pub build_id: Vec<u8>,
    pub sdk_version: String,
    pub compiler_identity: String,
    pub flags: String,
    pub workload_tag: String,
    pub abi: u8,
    pub consumer_mode: u8,
    pub session_id: u64,
    pub ring_records: u32,
    pub max_threads: u32,
    pub max_descriptors: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Clock {
    pub freq_hz: u64,
    pub tb_at_init: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Descriptor {
    pub index: u32,
    pub id: u64,
    pub scope: u8,
    pub line: u32,
    pub name: String,
    pub logical_tag: String,
    pub file: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ThreadInfo {
    pub incarnation: u32,
    pub slot: u32,
    pub os_thread_id: u64,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FinalEntry {
    pub incarnation: u32,
    pub state: u8,
    pub limit_flags: u8,
    pub produced_count: u64,
    pub written_count: u64,
    pub dropped_count: u64,
    pub refused_count: u64,
    pub regerr_count: u64,
    pub open_spans: u64,
    /// True when the entry came from a RETIRED chunk, false for the TRAILER.
    pub from_retired_chunk: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Trailer {
    pub end_flags: u32,
    pub sink_error_count: u64,
    pub first_sink_error: i32,
    pub unregistered_refusals: u64,
    pub descriptor_count: u64,
}

/// The frame context of a BEGIN on its own incarnation (section 9, frame attribution).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameContext {
    /// The last FRAME record before the BEGIN, in this incarnation's sequence order, had this id.
    Frame(u64),
    /// No FRAME record before the BEGIN and no GAP.
    PreFrame,
    /// A GAP came after the last FRAME (or before any): a FRAME may have been lost.
    Loss,
}

/// One span, as reconstructed from its BEGIN+BEGIN_PARENT and (if present) its END.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Span {
    pub incarnation: u32,
    pub span_id: u64,
    pub desc: u32,
    pub depth: u8,
    pub parent: u64,
    /// (owner incarnation, frame id) when the BEGIN_PARENT carries a token.
    pub token: Option<(u32, u64)>,
    pub begin_seq: u64,
    pub begin_tb: u64,
    pub end_tb: Option<u64>,
    pub truncated_at_stop: bool,
    /// Section 4: on the partial stack at a GAP, or begun while the base was UNKNOWN.
    pub affected: bool,
    pub frame_context: FrameContext,
    /// FRAME records seen on this incarnation before the BEGIN and before the END.
    pub frames_before_begin: u64,
    pub frames_before_end: Option<u64>,
    /// GAPs seen on this incarnation before the BEGIN and before the END.
    pub gaps_before_begin: u64,
    pub gaps_before_end: Option<u64>,
    /// The summed inclusive durations of this span's direct children on the same incarnation (u128: exact, no
    /// saturation).
    pub children_inclusive: u128,
    /// A direct child ended but its duration is unavailable (END tb before its BEGIN tb), so this span's exclusive
    /// duration is unavailable too.
    pub children_unavailable: bool,
}

impl Span {
    pub fn is_open(&self) -> bool {
        self.end_tb.is_none()
    }
    /// OPEN or TRUNCATED_AT_STOP (section 9).
    pub fn is_censored(&self) -> bool {
        self.is_open() || self.truncated_at_stop
    }
    /// END tb - BEGIN tb for a span that has an END.  None when OPEN, and None (unavailable, never 0) when the END tb
    /// is earlier than the BEGIN tb (a CLOCK_ANOMALY).
    pub fn inclusive(&self) -> Option<u64> {
        self.end_tb.and_then(|end| end.checked_sub(self.begin_tb))
    }
    /// The span ended, but END tb < BEGIN tb: its duration is unavailable.
    pub fn duration_unavailable(&self) -> bool {
        self.end_tb.is_some() && self.inclusive().is_none()
    }
    /// Inclusive minus the direct children's inclusive durations (section 9).  None when the inclusive duration, or
    /// any direct child's, is unavailable, or when the children exceed the parent (only possible with clock anomalies).
    pub fn exclusive(&self) -> Option<u64> {
        if self.children_unavailable {
            return None;
        }
        let inc = self.inclusive()?;
        u128::from(inc).checked_sub(self.children_inclusive).and_then(|x| u64::try_from(x).ok())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameRecord {
    pub id: u64,
    pub seq: u64,
    pub tb: u64,
}

/// Everything read for one incarnation.
#[derive(Debug, Clone, Default)]
pub struct Lane {
    pub info: Option<ThreadInfo>,
    /// Sequenced records written for this incarnation.
    pub written: u64,
    pub last_seq: Option<u64>,
    /// Sum of the GAP ranges, and the number of GAPs.
    pub gap_sum: u128,
    pub gap_count: u64,
    pub clock_anomaly: bool,
    pub frames: Vec<FrameRecord>,
    pub regerr_records: u64,
    /// ENDs tolerated on an empty partial stack with base UNKNOWN (section 4).
    pub affected_ends: u64,
    pub final_entry: Option<FinalEntry>,
    /// Indices into [`Capture::spans`].
    pub spans: Vec<usize>,

    // Parse state, per section 4.
    pub(crate) expected_seq: u64,
    pub(crate) pending_gap: Option<(u64, u64)>,
    pub(crate) pending_begin: Option<crate::reader::RawRecord>,
    pub(crate) stack: Vec<usize>,
    pub(crate) base_unknown: bool,
    pub(crate) span_ids: BTreeSet<u64>,
    pub(crate) last_tb: Option<u64>,
    pub(crate) next_frame_id: u64,
    /// The last FRAME id was u64::MAX, so no further id can be assigned (checked, never saturated).
    pub(crate) frame_ids_exhausted: bool,
    pub(crate) gap_since_frame: bool,
    pub(crate) current_frame: Option<u64>,
    pub(crate) retired: bool,
}

impl Lane {
    /// Owner of frames: an incarnation that emits FRAME records.
    pub fn is_frame_owner(&self) -> bool {
        !self.frames.is_empty()
    }
}

/// A decoded capture that is not MALFORMED.  It may be INCOMPLETE; then `incomplete` says why.
#[derive(Debug, Clone, Default)]
pub struct Capture {
    pub header: Option<Header>,
    pub clock: Option<Clock>,
    /// minor > 0: readable, with a warning (section 2).
    pub minor_warning: bool,
    pub descriptors: BTreeMap<u32, Descriptor>,
    pub lanes: BTreeMap<u32, Lane>,
    pub spans: Vec<Span>,
    pub trailer: Option<Trailer>,
    pub incomplete: Vec<Incomplete>,
}

impl Capture {
    pub fn has_trailer(&self) -> bool {
        self.trailer.is_some()
    }
    pub fn torn(&self) -> bool {
        self.incomplete.contains(&Incomplete::TornTail)
    }
}

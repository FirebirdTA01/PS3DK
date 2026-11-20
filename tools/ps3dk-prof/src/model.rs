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
/// file_flags bit 0: the file may carry domain-extension chunks (types 8..16).
pub const FLAG_DOMAIN_EXTENSIONS: u32 = 1;
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
pub const CHUNK_EXPORT: u32 = 40;
pub const EXPORT_SCHEMA: u16 = 1;
pub const EXPORT_FLAG_TO_END: u16 = 1;
/// The source is a report-ring capture (AUTO 2047/11): a 40-byte ring accounting tail follows the lane entries.
pub const EXPORT_FLAG_RING: u16 = 2;
pub const EXPORT_RING_LEN: usize = 40;
pub const EXPORT_HEADER_LEN: usize = 112;
pub const EXPORT_LANE_ENTRY_LEN: usize = 40;

/// Per-lane accounting entry inside an EXPORT chunk (40 bytes).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportLane {
    pub incarnation: u32,
    pub first_seq: u64,
    pub last_seq: u64,
    pub kept: u64,
    pub dropped_by_export: u64,
}

/// The ring accounting tail of an EXPORT chunk (EXPORT_FLAG_RING): the RING records the export kept are whole
/// generations, strictly increasing from `first_generation` to `last_generation` (`generations` of them,
/// `records_kept` records); `records_dropped` RING records of the source were not kept, and
/// `source_last_generation` is the source's last RING generation (what an exact source FINAL reconciled with the
/// STATUS blocks issued, which the export keeps as written). All zero but the source fields when nothing was kept.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExportRing {
    pub first_generation: u32,
    pub last_generation: u32,
    pub generations: u64,
    pub records_kept: u64,
    pub records_dropped: u64,
    pub source_last_generation: u32,
}

/// A parsed EXPORT chunk (type 40).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportRecord {
    pub schema: u16,
    pub flags: u16,
    pub source_crc: u32,
    pub source_size: u64,
    pub frame_first: u64,
    pub frame_last: u64,
    pub clipped_zones: u64,
    pub records_kept: u64,
    pub records_dropped: u64,
    pub ops_kept: u64,
    pub ops_dropped: u64,
    pub events_kept: u64,
    pub events_dropped: u64,
    pub source_fifo_last_attempt_seq: u64,
    pub source_auto_frames: u64,
    pub lanes: BTreeMap<u32, ExportLane>,
    /// EXPORT_FLAG_RING: the report-ring accounting tail.
    pub ring: Option<ExportRing>,
}

pub const CHUNK_WINDOW_BEGIN: u32 = 41;
pub const CHUNK_WINDOW_END: u32 = 42;

pub const FLAG_WINDOWS: u32 = 512;
pub const WINDOW_BEGIN_LEN: usize = 32;
pub const WINDOW_END_LEN: usize = 32;

pub const WINDOW_SOURCE_APP: u8 = 1;
pub const WINDOW_SOURCE_LIVE: u8 = 2;

pub const WINDOW_KIND_MANUAL: u8 = 1;
pub const WINDOW_KIND_FRAMES: u8 = 2;
pub const WINDOW_KIND_SECTION: u8 = 3;

pub const WINDOW_REASON_END: u8 = 1;
pub const WINDOW_REASON_COUNT: u8 = 2;
pub const WINDOW_REASON_SHUTDOWN: u8 = 3;
pub const WINDOW_REASON_SEAL: u8 = 4;
pub const WINDOW_REASON_REFUSED_CAPACITY: u8 = 5;

pub const KIND_BEGIN: u8 = 1;
pub const KIND_BEGIN_PARENT: u8 = 2;
pub const KIND_END: u8 = 3;
pub const KIND_FRAME: u8 = 4;
pub const KIND_GAP: u8 = 5;
pub const KIND_REGERR: u8 = 6;

pub const FLAG_TOKEN: u16 = 1;
pub const FLAG_TRUNCATED_AT_STOP: u16 = 1;
pub const GAP_FLAG_EXPORT: u16 = 1;

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
    /// Opt-in frame membership or closure violates its wire contract.
    Checkpoint,
    WaitConfig,
    WaitRecord,
    WaitStatus,
    /// Named per-frame counters (file_flags bit 8, chunk 39) violate their wire contract.
    Counters,
    /// Capture windows (file_flags bit 9, chunks 41-42) violate their wire contract.
    Windows,
    /// RSX frame captures (file_flags bit 10, chunks 44-48) violate their wire contract.
    RsxFrame,
    /// Function paths (file_flags bit 5, chunk types 28-30; ps3dk-prof-func-paths.md): FUNC_CONFIG.
    FuncConfig,
    /// A FUNC_TREE chunk violates its wire, ordering or definition rules.
    FuncTree,
    /// A FUNC_STATUS chunk violates its wire or ordering rules.
    FuncStatus,
    /// FIFO wrap events (file_flags bit 6, chunk types 31-33; ps3dk-prof-fifo.md): FIFO_CONFIG.
    FifoConfig,
    /// A FIFO_EVENTS chunk violates its wire or ordering rules.
    FifoEvents,
    /// A FIFO_STATUS chunk violates its wire, conservation or ordering rules.
    FifoStatus,
    /// Automatic RSX observation (file_flags bit 7, chunk types 34-38; ps3dk-prof-auto-rsx.md): AUTO_CONFIG.
    AutoConfig,
    /// An AUTO_OPS chunk violates its wire or ordering rules.
    AutoOps,
    /// An AUTO_COVERAGE row violates its wire, accounting or ordering rules.
    AutoCoverage,
    /// An AUTO_STATUS chunk violates its wire, conservation or ordering rules.
    AutoStatus,
    /// An AUTO_CLOCK anchor violates its wire or identity rules.
    AutoClock,
    /// An EXPORT chunk (type 40) violates its wire or range rules.
    Export,
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
    /// A rule of the domain extensions (chunk types 8..16, file_flags bit 0); the detail names it.
    Extension,
    /// PROFILE record (file_flags bit 1, chunk type 17; profile-format-spec.md 3): a payload rule, the detail names it.
    Profile,
    /// file_flags bit 1 is set and chunk 2 is not a PROFILE chunk.
    ProfileMissing,
    /// A PROFILE chunk at an index other than 2 (including a second one).
    ProfilePlacement,
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
    /// A file without a TRAILER whose whole chunks hold metadata only (HEADER, CLOCKS, tables): the process ended
    /// before the first drain wrote an event chunk.  Set by [`crate::load_file`], alone, in place of the strict
    /// reasons.
    NoEvents,
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
            Incomplete::NoEvents => write!(f, "no events recorded (ended before the first drain)"),
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
    pub end_seq: Option<u64>,
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
    pub first_seq: Option<u64>,
    pub last_seq: Option<u64>,
    /// Sum of the GAP ranges, and the number of GAPs.
    pub gap_sum: u128,
    pub gap_count: u64,
    pub export_gap_count: u64,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowSource {
    App = 1,
    Live = 2,
}

impl WindowSource {
    pub fn as_str(&self) -> &'static str {
        match self {
            WindowSource::App => "app",
            WindowSource::Live => "live",
        }
    }
}

impl fmt::Display for WindowSource {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowKind {
    Manual,
    Frames(u32),
    Section,
}

impl WindowKind {
    pub fn as_str(&self) -> String {
        match self {
            WindowKind::Manual => "manual".to_string(),
            WindowKind::Frames(n) => format!("frames({n})"),
            WindowKind::Section => "section".to_string(),
        }
    }
}

impl fmt::Display for WindowKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.as_str())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowReason {
    End = 1,
    Count = 2,
    Shutdown = 3,
    Seal = 4,
    RefusedCapacity = 5,
    OpenAtEnd = 6,
}

impl WindowReason {
    pub fn as_str(&self) -> &'static str {
        match self {
            WindowReason::End => "end",
            WindowReason::Count => "count",
            WindowReason::Shutdown => "shutdown",
            WindowReason::Seal => "seal",
            WindowReason::RefusedCapacity => "refused_capacity",
            WindowReason::OpenAtEnd => "open at end of capture",
        }
    }
}

impl fmt::Display for WindowReason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CaptureWindow {
    pub id: u32,
    pub name: String,
    pub name_desc: u32,
    pub first_frame: u64,
    pub last_frame: Option<u64>,
    pub source: WindowSource,
    pub kind: WindowKind,
    pub reason: WindowReason,
    pub is_open: bool,
    pub incomplete: bool,
}

impl CaptureWindow {
    pub fn contains_frame(&self, frame: u64) -> bool {
        match self.last_frame {
            Some(last) => frame >= self.first_frame && frame <= last,
            None => false,
        }
    }

    pub fn frames_desc(&self) -> String {
        match self.last_frame {
            Some(last) => format!("frames {}..{}", self.first_frame, last),
            None => "0 frames".to_string(),
        }
    }
}

/// A decoded capture that is not MALFORMED.  It may be INCOMPLETE; then `incomplete` says why.
#[derive(Debug, Clone, Default)]
pub struct Capture {
    pub(crate) dependencies: Option<crate::dependencies::DependencyIndex>,
    /// Opt-in inventory evidence, separate from legacy source validity and from
    /// completion of the work. None for files without file_flags bit 3.
    pub checkpoints: Option<crate::checkpoints::Inventory>,
    /// Opt-in source proof, independent of legacy validity. Admission alone
    /// never publishes a completed source result.
    pub source_checkpoints: Option<crate::source_checkpoints::Evidence>,
    pub waits: Option<crate::waits::Inventory>,
    /// Automatic function paths (file_flags bit 5); None without the capability.
    pub func_paths: Option<crate::func_paths::Inventory>,
    /// Named per-frame counters (file_flags bit 8); None without the capability.
    pub counters: Option<crate::counters::Inventory>,
    /// FIFO wrap phase events (file_flags bit 6): present exactly when the flag is.
    pub fifo: Option<crate::fifo::Inventory>,
    /// Automatic RSX observation (file_flags bit 7): present exactly when the flag is.
    pub auto: Option<crate::auto_rsx::Inventory>,
    /// RSX frame captures (file_flags bit 10): present exactly when the flag is.
    pub rsx_frame: Option<crate::rsx_frame::Inventory>,
    pub header: Option<Header>,
    pub clock: Option<Clock>,
    /// minor > 0: readable, with a warning (section 2).
    pub minor_warning: bool,
    pub descriptors: BTreeMap<u32, Descriptor>,
    pub lanes: BTreeMap<u32, Lane>,
    pub spans: Vec<Span>,
    pub trailer: Option<Trailer>,
    pub incomplete: Vec<Incomplete>,
    /// file_flags bit 9 (FLAG_WINDOWS): explicit capture windows, or 1 implicit window for legacy files.
    pub windows: Vec<CaptureWindow>,
    pub windows_flag: bool,
    /// file_flags bit 0 (DOMAIN_EXTENSIONS): the extension sources; None for a v1 file (flags 0).
    pub ext: Option<crate::ext::ExtCapture>,
    /// file_flags bit 1 (PROFILE_RECORD) is set.
    pub profile_flag: bool,
    /// The decoded PROFILE (chunk 2); None when bit 1 is clear or the file is torn before chunk 2 is complete.
    pub profile: Option<crate::profile::ProfileRecord>,
    /// Standalone export metadata (chunk 40); Some when exported from another capture.
    pub export: Option<ExportRecord>,
    /// Host-created bounded frame snapshot (chunk 49); not whole-run evidence.
    pub snapshot: Option<crate::snapshot_support::Selection>,
}

impl Capture {
    pub fn dependency_status(&self) -> Option<crate::dependencies::IndexStatus> {
        self.dependencies.as_ref().map(|i| i.status.clone())
    }
    pub fn dependency_record(&self, id: crate::dependencies::RecordId, expected: crate::dependencies::Expected,
        before_chunk: u64, budget: crate::dependencies::QueryBudget) -> crate::dependencies::Lookup<crate::dependencies::RecordFact> {
        self.dependencies.as_ref().map_or(crate::dependencies::Lookup::Unavailable(crate::dependencies::UnavailableReason::HistoryNotIndexed),
            |i| i.record(id, expected, before_chunk, budget))
    }
    pub fn dependency_pair(&self, begin: crate::dependencies::RecordId, expected: crate::dependencies::Expected,
        before_chunk: u64, budget: crate::dependencies::QueryBudget) -> crate::dependencies::Lookup<crate::dependencies::PairFact> {
        self.dependencies.as_ref().map_or(crate::dependencies::Lookup::Unavailable(crate::dependencies::UnavailableReason::HistoryNotIndexed),
            |i| i.pair(begin, expected, before_chunk, budget))
    }
    pub fn has_trailer(&self) -> bool {
        self.trailer.is_some()
    }
    pub fn torn(&self) -> bool {
        self.incomplete.contains(&Incomplete::TornTail)
    }
}

//! Automatic RSX observation (file_flags bit 7, chunk types 34-38; docs/design/ps3dk-prof-auto-rsx.md).
//!
//! Strict decode of the five AUTO chunks, then a conservative per-frame reading.  An operation interval is a
//! reached-marker elapsed interval on the RSX timestamp clock between two command-stream positions: not isolated draw
//! cost, not utilization and not proof that the RSX was continuously busy.  RSX ticks become PPU ticks only through a
//! map this capture qualified itself (its own CLOCK anchors, an exact affine feasible set and both H-SPLIT hold-out
//! parities); there is no nominal rate.  A frame verdict needs complete covered population, raw-valid operations, a
//! qualified map and wait evidence; anything missing is UNKNOWN with the missing input named.
use std::collections::{BTreeMap, BTreeSet};

use num_bigint::BigInt;
use num_traits::{Signed, ToPrimitive, Zero};

use crate::clockmap::{self, Limit, Meter, Obs, Rat, Solved};
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::rsx_measured::{split_budget, split_holdout, SplitOutcome};
use crate::wire::Cursor;

pub const FLAG_AUTO: u32 = 128;
pub const CHUNK_AUTO_CONFIG: u32 = 34;
pub const CHUNK_AUTO_OPS: u32 = 35;
pub const CHUNK_AUTO_COVERAGE: u32 = 36;
pub const CHUNK_AUTO_STATUS: u32 = 37;
pub const CHUNK_AUTO_CLOCK: u32 = 38;

pub const CONFIG_BYTES: usize = 160;
pub const OP_BYTES: u32 = 128;
/// OPS record width of the build-frame writer (CONFIG masks 63/11): the 128-byte record plus the build tail.
pub const OP_BYTES_BUILD: u32 = 144;
pub const FRAME_BYTES: usize = 352;
/// COVERAGE rows of the presentation writer (CONFIG masks 31/11): the 352-byte row plus a presentation block.
pub const FRAME_BYTES_PRESENT: usize = 384;
pub const STATUS_BYTES: usize = 128;
pub const CLOCK_BYTES: usize = 96;
/// Schema 1: the first sixteen reports of the lease are clock anchors.
pub const CLOCK_SLOTS: u32 = 16;
pub const CLOCK_POLLS: u32 = 100_000;
pub const MAX_ALLOCATION: u64 = 2 * 1024 * 1024;
pub const PAIRS_MAX: u32 = 504;

pub const CONFIG_REFUSED: u16 = 1;
pub const KIND_ARRAY: u16 = 1;
pub const KIND_INDEX: u16 = 2;
pub const KIND_CLEAR: u16 = 3;
/// D2: a frame boundary mark (args[0] 1 = frame begin, 2 = frame end); only with CONFIG kind mask bit 3.
pub const KIND_FRAME: u16 = 4;
pub const FRAME_BEGIN: u32 = 1;
pub const FRAME_END: u32 = 2;
/// CONFIG masks: the first writer (no FRAME, CALL unsupported) and the D1/D2 writer (FRAME, CALL segments).
pub const MASKS_V1: (u32, u32) = (7, 15);
pub const MASKS_SEGMENTS: (u32, u32) = (15, 11);
/// The presentation writer: kind mask bit 4 (16) PRESENT names flip-wait scopes and polling cadence in 384-byte
/// COVERAGE rows.  It is not an OPS kind.  Readers that know only 7/15 or 15/11 refuse it at CONFIG.
pub const MASKS_PRESENT: (u32, u32) = (31, 11);
/// The build-frame writer: kind mask bit 5 (32) SEGMENT_BUILD.  SEGMENT records carry the frame that built them in a
/// 144-byte OPS record (u64@128 build frame, u32@136 build owner, u16@140 build flags, u16@142 zero) and COVERAGE
/// u32@380 counts segment draws a CALL executed without a record.  Not an OPS kind.  Readers that know only the older
/// pairs refuse it at CONFIG.
pub const MASKS_BUILD: (u32, u32) = (63, 11);
/// Build tail flag: the draw was admitted while a coverage frame was open (else build owner and frame are zero).
pub const OP_BUILD_KNOWN: u16 = 1;
pub const OP_TOKEN: u16 = 1;
pub const OP_RAW_VALID: u16 = 2;
pub const OP_INCOMPLETE: u16 = 4;
pub const OP_CLOCK_INVALID: u16 = 8;
pub const OP_READ_REFUSED: u16 = 16;
pub const OP_PUBLICATION_UNKNOWN: u16 = 32;
/// D1: built inside a CALL segment; u32@92 segment ordinal, u32@116 call ordinal.  Only when CONFIG covers CALL.
pub const OP_SEGMENT: u16 = 64;
pub const FR_TOKEN: u16 = 1;
pub const FR_FINAL: u16 = 2;
pub const FR_SATURATED: u16 = 4;
pub const FR_LOSS: u16 = 8;
pub const FR_OPEN: u16 = 16;
/// The frame was opened by a flip queued on the owner thread (15/11 CONFIG masks only).
pub const FR_FLIP: u16 = 32;
/// The writer observed presentation in this frame.  With the 15/11 CONFIG masks: u32@12 flip-status poll intervals
/// and u64@56 the owner PPU ticks between the first pending poll and the done poll (an observation interval, never
/// waiting evidence).  With 31/11: u32@12 flip-wait scopes and u64@56 their elapsed ticks, plus the presentation block
/// at 352 (polling cadence).  Without it there is no presentation evidence.
pub const FR_PRESENT: u16 = 64;
pub const ST_FINAL: u16 = 1;
pub const ST_LOWER_BOUND: u16 = 2;
pub const ST_SATURATED: u16 = 4;
pub const ST_OWNER_UNACK: u16 = 8;
pub const ST_RESOURCE_CONFLICT: u16 = 16;
pub const ST_CONTEXT_RESET: u16 = 32;
/// D1: report pairs of a segment build that can never complete were quarantined (held_report_pairs counts them).
pub const ST_SEGMENT_QUARANTINE: u16 = 64;
/// D2: at least one FRAME mark was refused (room or resources); frames without both marks have no RSX duration.
pub const ST_FRAME_MARK_MISSED: u16 = 128;
/// Some frames were delimited by flips the GCM layer saw on the owner thread.
pub const ST_FRAMES_FLIP: u16 = 256;
/// Some frames were delimited by explicit ps3dk_prof_frame calls (which then take precedence over flips).
pub const ST_FRAMES_EXPLICIT: u16 = 512;
/// 63/11: a CALL segment whose build carried timed draws was called again without a rebuild.  Its embedded report
/// writes ran again and may have overwritten other records' timestamps, so no frame of the capture has a verdict.
pub const ST_SEGMENT_REPLAY: u16 = 1024;
pub const CK_VALID: u16 = 1;
pub const CK_TIMEOUT: u16 = 2;
pub const CK_REGRESSION: u16 = 4;
pub const CK_READ_REFUSED: u16 = 8;

/// REFUSED CONFIG reasons 1..10, in wire order.
pub const REASONS: [&str; 10] = [
    "GCM_INIT_FAILED",
    "NO_INDEPENDENT_DRAIN",
    "RESOURCE_LEASE_UNVERIFIED",
    "PRIOR_WRITES_UNKNOWN",
    "RESOURCE_CONFLICT",
    "NO_REGISTERED_OWNER",
    "ALLOCATION_FAILED",
    "UNSUPPORTED_CONTEXT",
    "STOPPED",
    "NO_GCM_INIT_OBSERVED",
];
/// COVERAGE row columns after visited and observed.
pub const EXCLUSIONS: [&str; 10] = [
    "room",
    "report capacity",
    "metadata capacity",
    "reentry",
    "unsupported arguments",
    "shutdown",
    "ownership uncertainty",
    "observer disabled",
    "resource lease unverified",
    "epoch invalid",
];
pub const KINDS: [&str; 3] = ["ARRAY", "INDEX", "CLEAR"];
/// STATUS counters from offset 8, in wire order.
pub const STATUS_FIELDS: [&str; 15] = [
    "last_admitted_id",
    "admitted",
    "operations_written",
    "operations_dropped",
    "pending_unreported",
    "frames_written",
    "frames_lost",
    "unassociated_visits",
    "internal_suppressed",
    "clock_attempted",
    "clock_written",
    "clock_lost",
    "resource_conflicts",
    "last_published_serial",
    "held_report_pairs",
];

/// Retention ceilings.  Parsing (and every wire rule) continues past them; only retention stops, with a named refusal.
#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub max_ops: u64,
    pub max_frames: u64,
    pub max_clocks: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self { max_ops: 1 << 22, max_frames: 1 << 20, max_clocks: 1 << 16 }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Attached {
    pub instance: u32,
    pub domain: u32,
    pub epoch: u32,
    pub owner: u32,
    pub generation: u32,
    pub report_first: u32,
    pub report_count: u32,
    pub op_label: u32,
    pub clock_label: u32,
    pub pairs: u32,
    pub metadata: u32,
    pub frame_capacity: u32,
    /// Covered kinds (bit k-1 for kind k) and unsupported forms (unsafe 1, immediate 2, CALL 4, direct 8).
    pub kind_mask: u32,
    pub form_mask: u32,
    pub frequency: u64,
    pub allocated: u64,
    pub ceiling: u64,
    pub mechanism: [u8; 32],
    pub binding: [u8; 32],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Config {
    Attached(Box<Attached>),
    Refused { instance: u32, reason: u32, application_rc: i32, frequency: u64, mechanism: [u8; 32], binding: [u8; 32] },
}
impl Attached {
    pub fn frames_marked(&self) -> bool {
        self.kind_mask & 8 != 0
    }
    pub fn segments_covered(&self) -> bool {
        self.form_mask & 4 == 0
    }
    /// The writer records flip-wait scopes and polling cadence (CONFIG kind mask bit 16, 384-byte rows).
    pub fn presentation_scopes(&self) -> bool {
        self.kind_mask & 16 != 0
    }
    /// SEGMENT records name their building frame and rows count unrepresented CALL draws (CONFIG 63/11).
    pub fn build_frames(&self) -> bool {
        self.kind_mask & 32 != 0
    }
    /// The OPS record width this CONFIG negotiated.
    pub fn op_width(&self) -> u32 {
        if self.build_frames() { OP_BYTES_BUILD } else { OP_BYTES }
    }
}
impl Config {
    pub fn instance(&self) -> u32 {
        match self {
            Config::Attached(a) => a.instance,
            Config::Refused { instance, .. } => *instance,
        }
    }
    pub fn attached(&self) -> Option<&Attached> {
        match self {
            Config::Attached(a) => Some(a),
            Config::Refused { .. } => None,
        }
    }
}

pub fn reason_name(reason: u32) -> &'static str {
    REASONS.get(reason.wrapping_sub(1) as usize).copied().unwrap_or("UNKNOWN")
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Op {
    pub id: u64,
    pub frame_owner: u32,
    pub kind: u16,
    pub flags: u16,
    pub frame: u64,
    pub tb_before: u64,
    pub tb_after: u64,
    pub raw_begin: u64,
    pub raw_end: u64,
    pub caller: u64,
    /// words, start, count, offset, index type, mode, clear mask
    pub args: [u32; 7],
    pub report_begin: u32,
    pub report_end: u32,
    pub serial_begin: u32,
    pub serial_end: u32,
    pub generation: u32,
    pub published: u64,
    /// SEGMENT only: segment ordinal and call ordinal (0 for a build that was never called).
    pub segment: u32,
    pub call: u32,
    /// SEGMENT records of a 63/11 capture: the (owner, frame) whose coverage row counted this draw when it was built,
    /// None when no frame was open then (and always None for other records and older captures).
    pub build: Option<(u32, u64)>,
}
impl Op {
    pub fn raw_valid(&self) -> bool {
        self.flags & OP_RAW_VALID != 0
    }
    pub fn token(&self) -> Option<(u32, u64)> {
        (self.flags & OP_TOKEN != 0).then_some((self.frame_owner, self.frame))
    }
    /// A covered draw or clear (kinds 1-3), as opposed to a FRAME mark.
    pub fn covered(&self) -> bool {
        (KIND_ARRAY..=KIND_CLEAR).contains(&self.kind)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Coverage {
    pub flags: u16,
    pub owner: u32,
    pub frame: u64,
    pub tb_begin: u64,
    pub tb_end: u64,
    pub unsupported: u64,
    pub lost: u64,
    /// ARRAY, INDEX, CLEAR: visited, observed, ten exclusions.
    pub rows: [[u64; 12]; 3],
    /// FR_PRESENT rows of the presentation writer (31/11): flip-wait scopes completed inside the frame and their
    /// elapsed owner PPU ticks (including scheduling and polling), and the polling cadence block.
    pub flip_waits: u32,
    pub flip_wait: u64,
    pub flip_wait_spanning: u32,
    pub flip_wait_timeouts: u32,
    pub cadence: Cadence,
    /// FR_PRESENT rows of the 15/11 writer: flip-status poll intervals and their ticks (observation only).
    pub legacy_polls: Option<(u32, u64)>,
    /// 63/11 rows (u32@380): covered segment draws that CALLs in this frame executed without a committed record
    /// carrying the CALL's token.  A lower bound (an untracked segment counts 1); zero in older captures.
    pub segment_unrepresented: u32,
}

/// Polling cadence of one frame: runs of consecutive pending flip-status polls with every gap within 1 ms.  It shows
/// how often the owner looked at the flip, never that it waited.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Cadence {
    pub runs: u32,
    pub broken: u32,
    pub spanning: u32,
    pub ticks: u64,
}
impl Coverage {
    /// The kind's ten exclusions for display, saturating; verdicts use `excluded_checked`.
    pub fn excluded(&self, kind: usize) -> u64 {
        self.rows[kind][2..].iter().fold(0u64, |a, &b| a.saturating_add(b))
    }
    /// The kind's ten exclusions, or None when their sum does not fit in u64 (possible only in a SATURATED row,
    /// whose accounting the wire does not reconcile).
    pub fn excluded_checked(&self, kind: usize) -> Option<u64> {
        self.rows[kind][2..].iter().try_fold(0u64, |a, &b| a.checked_add(b))
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    pub flags: u16,
    pub counters: [u64; 15],
}
impl Status {
    pub fn is_final(&self) -> bool {
        self.flags & ST_FINAL != 0
    }
    pub fn lower_bound(&self) -> bool {
        self.flags & ST_LOWER_BOUND != 0
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Anchor {
    pub flags: u16,
    pub epoch: u32,
    pub id: u32,
    pub role: u32,
    pub slot: u32,
    pub serial: u32,
    pub polls: u32,
    pub tb_before: u64,
    pub tb_after: u64,
    pub raw: u64,
    pub repeated: u64,
    pub label_first: u32,
    pub label_second: u32,
    pub frame: u64,
    pub owner: u32,
    pub elapsed: u64,
}
impl Anchor {
    pub fn outcome(&self) -> &'static str {
        match self.flags {
            CK_VALID => "VALID",
            CK_TIMEOUT => "TIMEOUT",
            CK_REGRESSION => "CLOCK_REGRESSION",
            _ => "READ_REFUSED",
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Inventory {
    pub config: Option<Config>,
    pub ops: Vec<Op>,
    pub ops_parsed: u64,
    pub frames: Vec<Coverage>,
    pub frames_parsed: u64,
    pub clocks: Vec<Anchor>,
    pub clocks_parsed: u64,
    pub statuses: u64,
    pub last_status: Option<Status>,
    pub final_seen: bool,
    pub refusal: Option<&'static str>,
    /// The largest publication snapshot of any parsed OPS record.
    pub max_snapshot: u64,
}
impl Inventory {
    pub fn state(&self) -> String {
        match (&self.config, &self.last_status) {
            (None, _) => "INCOMPLETE: AUTO_CONFIG missing".into(),
            (Some(Config::Refused { reason, .. }), _) => {
                format!("automatic observation unavailable: {}", reason_name(*reason))
            }
            _ if self.refusal.is_some() => "REFUSED".into(),
            (_, None) => "PROVISIONAL: no FINAL status".into(),
            (_, Some(s)) if !s.is_final() => "PROVISIONAL: no FINAL status".into(),
            (_, Some(s)) if s.lower_bound() => "FINAL (lower bounds)".into(),
            _ => "FINAL".into(),
        }
    }
}

pub struct Reader {
    pub result: Inventory,
    budgets: Budgets,
    last_op: u64,
    last_frame: Option<(u32, u64)>,
    finals: BTreeSet<(u32, u64)>,
    last_lost: u64,
    last_anchor: u32,
}

fn bad<T>(rule: Rule, detail: impl Into<String>) -> Result<T, Malformed> {
    malformed(rule, detail)
}

fn label_valid(n: u32) -> bool {
    n == 64 || (69..=254).contains(&n)
}

impl Reader {
    pub fn new(budgets: Budgets) -> Self {
        Self {
            result: Inventory::default(),
            budgets,
            last_op: 0,
            last_frame: None,
            finals: BTreeSet::new(),
            last_lost: 0,
            last_anchor: 0,
        }
    }

    pub(crate) fn chunk(
        &mut self,
        ty: u32,
        payload: &[u8],
        cap: &Capture,
        rsx_domains: &BTreeSet<u32>,
        declared: &BTreeSet<u32>,
    ) -> Result<(), Malformed> {
        let r = rule_of(ty);
        if self.result.final_seen {
            return bad(r, format!("AUTO chunk type {ty} after the FINAL status"));
        }
        if ty != CHUNK_AUTO_CONFIG && self.result.config.is_none() {
            return bad(r, format!("AUTO chunk type {ty} before AUTO_CONFIG"));
        }
        if matches!(self.result.config, Some(Config::Refused { .. })) && ty != CHUNK_AUTO_STATUS {
            return bad(r, format!("AUTO chunk type {ty} after a REFUSED AUTO_CONFIG (only its FINAL status may follow)"));
        }
        match ty {
            CHUNK_AUTO_CONFIG => self.config(payload, cap, rsx_domains, declared),
            CHUNK_AUTO_OPS => self.ops(payload),
            CHUNK_AUTO_COVERAGE => self.coverage(payload),
            CHUNK_AUTO_STATUS => self.status(payload),
            CHUNK_AUTO_CLOCK => self.clock(payload),
            _ => unreachable!("AUTO chunk type {ty}"),
        }
    }

    fn attached(&self) -> &Attached {
        self.result.config.as_ref().and_then(Config::attached).expect("checked by chunk()")
    }

    fn config(&mut self, payload: &[u8], cap: &Capture, rsx: &BTreeSet<u32>, declared: &BTreeSet<u32>)
        -> Result<(), Malformed> {
        let r = Rule::AutoConfig;
        if self.result.config.is_some() {
            return bad(r, "a second AUTO_CONFIG (one instance per capture)");
        }
        if payload.len() != CONFIG_BYTES {
            return bad(r, format!("AUTO_CONFIG is {} bytes, not {CONFIG_BYTES}", payload.len()));
        }
        let mut c = Cursor::new(payload, "AUTO_CONFIG");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let mut w = [0u32; 14];
        for v in w.iter_mut() {
            *v = c.u32()?;
        }
        let frequency = c.u64()?;
        let allocated = c.u64()?;
        let ceiling = c.u64()?;
        let premise = c.u32()?;
        let lease = c.u32()?;
        let mut mechanism = [0u8; 32];
        mechanism.copy_from_slice(c.take(32)?);
        let mut binding = [0u8; 32];
        binding.copy_from_slice(c.take(32)?);
        c.done()?;
        if schema != 1 {
            return bad(r, format!("AUTO_CONFIG schema {schema}"));
        }
        if instance != 1 {
            return bad(r, format!("AUTO_CONFIG instance {instance} (schema 1 has one instance, 1)"));
        }
        if Some(frequency) != cap.clock.as_ref().map(|k| k.freq_hz) {
            return bad(r, format!("AUTO_CONFIG frequency {frequency} is not the CLOCKS frequency"));
        }
        let [domain, epoch, owner, generation, kinds, forms, first, count, op_label, clock_label, pairs, metadata,
            frame_capacity, width] = w;
        match flags {
            CONFIG_REFUSED => {
                let reason = kinds;
                if !(1..=10).contains(&reason) {
                    return bad(r, format!("REFUSED AUTO_CONFIG reason {reason}"));
                }
                if domain | epoch | owner | generation != 0
                    || w[6..].iter().any(|&x| x != 0)
                    || allocated | ceiling != 0
                    || premise | lease != 0
                {
                    return bad(r, "REFUSED AUTO_CONFIG carries a field other than reason, return code, frequency and digests");
                }
                self.result.config = Some(Config::Refused {
                    instance,
                    reason,
                    application_rc: forms as i32,
                    frequency,
                    mechanism,
                    binding,
                });
                Ok(())
            }
            0 => {
                if domain == 0 || epoch == 0 || owner == 0 || generation == 0 {
                    return bad(r, "AUTO_CONFIG domain, epoch, owner and generation must be nonzero");
                }
                if (kinds, forms) != MASKS_V1 && (kinds, forms) != MASKS_SEGMENTS && (kinds, forms) != MASKS_PRESENT
                    && (kinds, forms) != MASKS_BUILD
                {
                    return bad(r, format!("AUTO_CONFIG kind mask {kinds:#x} / unsupported-form mask {forms:#x}"));
                }
                if pairs == 0 || pairs > PAIRS_MAX || u64::from(count) != 16 + 2 * u64::from(pairs) {
                    return bad(r, format!("AUTO_CONFIG report count {count} is not 16 + 2 x {pairs} pairs"));
                }
                if u64::from(first) + u64::from(count) > 2048 {
                    return bad(r, format!("AUTO_CONFIG reports {first}+{count} beyond 2048"));
                }
                if !label_valid(op_label) || !label_valid(clock_label) || op_label == clock_label {
                    return bad(r, format!("AUTO_CONFIG labels {op_label}/{clock_label}"));
                }
                if !(1..=1024).contains(&metadata) || !(1..=256).contains(&frame_capacity) {
                    return bad(r, format!("AUTO_CONFIG capacities metadata {metadata} frames {frame_capacity}"));
                }
                // the record width is negotiated with the masks: 144 with 63/11 and only there
                let negotiated = if (kinds, forms) == MASKS_BUILD { OP_BYTES_BUILD } else { OP_BYTES };
                if width != negotiated {
                    return bad(r, format!("AUTO_CONFIG record width {width} with masks {kinds}/{forms}"));
                }
                if ceiling == 0 || ceiling > MAX_ALLOCATION || allocated == 0 || allocated > ceiling {
                    return bad(r, format!("AUTO_CONFIG allocated {allocated} / ceiling {ceiling}"));
                }
                if premise != 3 || lease != 1 {
                    return bad(r, format!("AUTO_CONFIG premise mask {premise:#x} / lease policy {lease}"));
                }
                if !rsx.contains(&domain) {
                    return bad(r, format!("AUTO_CONFIG domain {domain} is not a declared RSX_TIMESTAMP domain"));
                }
                if !declared.contains(&owner) {
                    return bad(r, format!("AUTO_CONFIG owner incarnation {owner} is not declared"));
                }
                self.result.config = Some(Config::Attached(Box::new(Attached {
                    instance,
                    domain,
                    epoch,
                    owner,
                    generation,
                    report_first: first,
                    report_count: count,
                    op_label,
                    clock_label,
                    pairs,
                    metadata,
                    frame_capacity,
                    kind_mask: kinds,
                    form_mask: forms,
                    frequency,
                    allocated,
                    ceiling,
                    mechanism,
                    binding,
                })));
                Ok(())
            }
            _ => bad(r, format!("AUTO_CONFIG variant {flags}")),
        }
    }

    fn ops(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::AutoOps;
        let cfg = self.attached().clone();
        if payload.len() < 16 {
            return bad(r, "AUTO_OPS shorter than its prefix");
        }
        let mut c = Cursor::new(payload, "AUTO_OPS");
        let schema = c.u16()?;
        let reserved = c.u16()?;
        let instance = c.u32()?;
        let count = c.u32()?;
        let width = c.u32()?;
        let stride = cfg.op_width();
        if schema != 1 || reserved != 0 || instance != cfg.instance || width != stride || count == 0 {
            return bad(r, format!("AUTO_OPS prefix schema {schema} reserved {reserved} instance {instance} \
                count {count} width {width}"));
        }
        if payload.len() as u64 != 16 + u64::from(count) * u64::from(stride) {
            return bad(r, format!("AUTO_OPS is {} bytes for {count} records", payload.len()));
        }
        let op_first = cfg.report_first + CLOCK_SLOTS;
        let op_end = cfg.report_first + cfg.report_count;
        for _ in 0..count {
            let id = c.u64()?;
            let frame_owner = c.u32()?;
            let kind = c.u16()?;
            let flags = c.u16()?;
            let frame = c.u64()?;
            let tb_before = c.u64()?;
            let tb_after = c.u64()?;
            let raw_begin = c.u64()?;
            let raw_end = c.u64()?;
            let caller = c.u64()?;
            let mut args = [0u32; 7];
            for a in args.iter_mut() {
                *a = c.u32()?;
            }
            let r0 = c.u32()?;
            let report_begin = c.u32()?;
            let report_end = c.u32()?;
            let serial_begin = c.u32()?;
            let serial_end = c.u32()?;
            let generation = c.u32()?;
            let r1 = c.u32()?;
            let published = c.u64()?;
            let (build_frame, build_owner, build_flags, build_reserved) = if stride == OP_BYTES_BUILD {
                (c.u64()?, c.u32()?, c.u16()?, c.u16()?)
            } else {
                (0, 0, 0, 0)
            };
            let at = format!("AUTO operation {id}");
            if id <= self.last_op {
                return bad(r, format!("{at}: id not above {}", self.last_op));
            }
            let known = if cfg.segments_covered() { 127 } else { 63 };
            if flags & !known != 0 {
                return bad(r, format!("{at}: unknown flags {flags:#x}"));
            }
            let segment_op = flags & OP_SEGMENT != 0;
            if segment_op {
                // r0 / r1 are the segment and call ordinals
                if r0 == 0 || (r1 == 0 && flags & OP_INCOMPLETE == 0) {
                    return bad(r, format!("{at}: SEGMENT without its segment or call ordinal"));
                }
            } else if r0 != 0 || r1 != 0 {
                return bad(r, format!("{at}: reserved field nonzero"));
            }
            // the build tail (63/11): flags mask 1, reserved zero; known names the CONFIG owner (frame 0 is valid);
            // unknown carries neither owner nor frame; a non-SEGMENT record carries no tail at all
            if build_flags & !OP_BUILD_KNOWN != 0 || build_reserved != 0 {
                return bad(r, format!("{at}: build tail flags {build_flags:#x} reserved {build_reserved}"));
            }
            let build_known = build_flags & OP_BUILD_KNOWN != 0;
            if !segment_op && (build_known || build_owner != 0 || build_frame != 0) {
                return bad(r, format!("{at}: build tail on a record not built in a segment"));
            }
            if build_known && build_owner != cfg.owner {
                return bad(r, format!("{at}: build owner {build_owner} is not the CONFIG owner"));
            }
            if !build_known && (build_owner != 0 || build_frame != 0) {
                return bad(r, format!("{at}: build owner/frame without BUILD_KNOWN"));
            }
            // a draw is built no later than the CALL that records it; a dropped build has no CALL token to compare
            if build_known && flags & OP_TOKEN != 0 && frame_owner == build_owner && build_frame > frame {
                return bad(r, format!("{at}: build frame {build_frame} after its CALL frame {frame}"));
            }
            let frame_kind = kind == KIND_FRAME && cfg.frames_marked();
            if !(KIND_ARRAY..=KIND_CLEAR).contains(&kind) && !frame_kind {
                return bad(r, format!("{at}: kind {kind}"));
            }
            if frame_kind && (segment_op || flags & OP_TOKEN == 0 || !(FRAME_BEGIN..=FRAME_END).contains(&args[0])
                || args[1..].iter().any(|&a| a != 0))
            {
                return bad(r, format!("{at}: FRAME mark needs a token, role 1 or 2 and no other argument"));
            }
            let valid = flags & OP_RAW_VALID != 0;
            let incomplete = flags & OP_INCOMPLETE != 0;
            if valid == incomplete {
                return bad(r, format!("{at}: exactly one of RAW_VALID and INCOMPLETE"));
            }
            if valid && flags & (OP_READ_REFUSED | OP_PUBLICATION_UNKNOWN) != 0 {
                return bad(r, format!("{at}: RAW_VALID with READ_REFUSED or PUBLICATION_UNKNOWN"));
            }
            if flags & OP_PUBLICATION_UNKNOWN != 0 && published != 0 {
                return bad(r, format!("{at}: PUBLICATION_UNKNOWN with a snapshot"));
            }
            if published > u64::from(u32::MAX) {
                return bad(r, format!("{at}: publication snapshot {published}"));
            }
            if flags & OP_TOKEN != 0 {
                if frame_owner != cfg.owner {
                    return bad(r, format!("{at}: frame owner {frame_owner} is not the CONFIG owner"));
                }
            } else if frame_owner != 0 || frame != 0 {
                return bad(r, format!("{at}: frame fields without TOKEN"));
            }
            if tb_after < tb_before && flags & OP_CLOCK_INVALID == 0 {
                return bad(r, format!("{at}: reversed emission bracket without CLOCK_INVALID"));
            }
            if generation != cfg.generation {
                return bad(r, format!("{at}: generation {generation}"));
            }
            if args[0] == 0 {
                return bad(r, format!("{at}: zero application words"));
            }
            let used: &[usize] = match kind {
                KIND_ARRAY => &[1, 2, 5],
                KIND_INDEX => &[2, 3, 4, 5],
                KIND_FRAME => &[],
                _ => &[6],
            };
            if (1..7).any(|j| !used.contains(&j) && args[j] != 0) {
                return bad(r, format!("{at}: argument unused by kind {kind} nonzero"));
            }
            if !(op_first..op_end).contains(&report_begin) || !(op_first..op_end).contains(&report_end)
                || report_begin == report_end
            {
                return bad(r, format!("{at}: reports {report_begin}/{report_end} outside the operation range"));
            }
            if serial_begin == 0 || serial_begin == u32::MAX || serial_end != serial_begin + 1 {
                return bad(r, format!("{at}: serials {serial_begin}/{serial_end}"));
            }
            if valid {
                if raw_begin > raw_end {
                    return bad(r, format!("{at}: raw begin above raw end"));
                }
                if published < u64::from(serial_end) {
                    return bad(r, format!("{at}: RAW_VALID publication snapshot {published} below serial {serial_end}"));
                }
            } else if raw_begin != 0 || raw_end != 0 {
                return bad(r, format!("{at}: INCOMPLETE with raw timestamps"));
            }
            self.last_op = id;
            self.result.ops_parsed += 1;
            self.result.max_snapshot = self.result.max_snapshot.max(published);
            if self.result.ops.len() as u64 >= self.budgets.max_ops {
                self.result.refusal.get_or_insert("AUTO operation retention ceiling");
                continue;
            }
            self.result.ops.push(Op {
                id,
                frame_owner,
                kind,
                flags,
                frame,
                tb_before,
                tb_after,
                raw_begin,
                raw_end,
                caller,
                args,
                report_begin,
                report_end,
                serial_begin,
                serial_end,
                generation,
                published,
                segment: if segment_op { r0 } else { 0 },
                call: if segment_op { r1 } else { 0 },
                build: build_known.then_some((build_owner, build_frame)),
            });
        }
        c.done()
    }

    fn coverage(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::AutoCoverage;
        let cfg = self.attached().clone();
        let width = if cfg.presentation_scopes() { FRAME_BYTES_PRESENT } else { FRAME_BYTES };
        if payload.len() != width {
            return bad(r, format!("AUTO_COVERAGE is {} bytes, not {width}", payload.len()));
        }
        let mut c = Cursor::new(payload, "AUTO_COVERAGE");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let owner = c.u32()?;
        let r0 = c.u32()?;
        let frame = c.u64()?;
        let tb_begin = c.u64()?;
        let tb_end = c.u64()?;
        let unsupported = c.u64()?;
        let lost = c.u64()?;
        let r1 = c.u64()?;
        let mut rows = [[0u64; 12]; 3];
        for row in rows.iter_mut() {
            for v in row.iter_mut() {
                *v = c.u64()?;
            }
        }
        let mut cadence = Cadence::default();
        let (mut scope_spanning, mut timeouts, mut r2) = (0u32, 0u32, 0u32);
        if cfg.presentation_scopes() {
            cadence.runs = c.u32()?;
            cadence.broken = c.u32()?;
            cadence.spanning = c.u32()?;
            scope_spanning = c.u32()?;
            cadence.ticks = c.u64()?;
            timeouts = c.u32()?;
            r2 = c.u32()?;
        }
        c.done()?;
        let at = format!("AUTO coverage frame {owner}/{frame}");
        let present = flags & FR_PRESENT != 0;
        // u32@380 is the 63/11 unrepresented-CALL count; reserved zero under every older pair
        if (r2 != 0 && !cfg.build_frames())
            || (!present && (cadence != Cadence::default() || scope_spanning != 0 || timeouts != 0))
        {
            return bad(r, format!("{at}: presentation block without PRESENT or a reserved field"));
        }
        if cadence.runs == 0 && cadence.ticks != 0 {
            return bad(r, format!("{at}: polling cadence {} ticks without a complete run", cadence.ticks));
        }
        if timeouts > r0 {
            return bad(r, format!("{at}: {timeouts} timed-out flip-wait scope(s) of {r0}"));
        }
        if schema != 1 || instance != cfg.instance || (!present && (r0 != 0 || r1 != 0)) {
            return bad(r, format!("{at}: schema {schema} instance {instance} or a reserved field"));
        }
        let known: u16 = if cfg.segments_covered() { 127 } else { 31 };
        if flags & !known != 0 || flags & FR_FINAL == 0 {
            return bad(r, format!("{at}: flags {flags:#x} (schema 1 rows are FINAL)"));
        }
        if flags & FR_TOKEN != 0 {
            if owner != cfg.owner {
                return bad(r, format!("{at}: owner is not the CONFIG owner"));
            }
        } else if owner != 0 {
            return bad(r, format!("{at}: owner without TOKEN"));
        }
        if !self.finals.insert((owner, frame)) {
            return bad(r, format!("{at}: a second final row"));
        }
        if let Some((o, f)) = self.last_frame {
            if o == owner && frame <= f {
                return bad(r, format!("{at}: frame not above {f}"));
            }
        }
        if present && r0 == 0 && r1 != 0 {
            return bad(r, format!("{at}: presentation count {r0} with {r1} ticks"));
        }
        if lost < self.last_lost || (lost != 0) != (flags & FR_LOSS != 0) {
            return bad(r, format!("{at}: lost count {lost} against LOSS / earlier {}", self.last_lost));
        }
        if flags & FR_OPEN == 0 && tb_end < tb_begin {
            return bad(r, format!("{at}: reversed frame clock without OPEN"));
        }
        if flags & FR_SATURATED == 0 {
            for (k, row) in rows.iter().enumerate() {
                let sum = row[1..].iter().try_fold(0u64, |a, &b| a.checked_add(b));
                if sum != Some(row[0]) {
                    return bad(r, format!("{at}: {} visited {} is not observed plus exclusions", KINDS[k], row[0]));
                }
            }
        }
        self.last_frame = Some((owner, frame));
        self.last_lost = lost;
        self.result.frames_parsed += 1;
        if self.result.frames.len() as u64 >= self.budgets.max_frames {
            self.result.refusal.get_or_insert("AUTO coverage retention ceiling");
            return Ok(());
        }
        let scopes = cfg.presentation_scopes();
        self.result.frames.push(Coverage { flags, owner, frame, tb_begin, tb_end, unsupported, lost, rows,
            flip_waits: if scopes { r0 } else { 0 }, flip_wait: if scopes { r1 } else { 0 },
            flip_wait_spanning: scope_spanning, flip_wait_timeouts: timeouts, cadence,
            legacy_polls: (present && !scopes).then_some((r0, r1)),
            segment_unrepresented: if cfg.build_frames() { r2 } else { 0 } });
        Ok(())
    }

    fn status(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::AutoStatus;
        if payload.len() != STATUS_BYTES {
            return bad(r, format!("AUTO_STATUS is {} bytes, not {STATUS_BYTES}", payload.len()));
        }
        let mut c = Cursor::new(payload, "AUTO_STATUS");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let mut n = [0u64; 15];
        for v in n.iter_mut() {
            *v = c.u64()?;
        }
        c.done()?;
        let cfg = self.result.config.clone().expect("checked by chunk()");
        let known = match cfg.attached() {
            Some(a) if a.build_frames() => 2047,
            Some(a) if a.segments_covered() => 1023,
            _ => 63,
        };
        if schema != 1 || instance != cfg.instance() || flags & !known != 0 {
            return bad(r, format!("AUTO_STATUS schema {schema} instance {instance} flags {flags:#x}"));
        }
        let fin = flags & ST_FINAL != 0;
        let Some(a) = cfg.attached() else {
            if flags != ST_FINAL || n.iter().any(|&x| x != 0) {
                return bad(r, "the status after a REFUSED AUTO_CONFIG is FINAL with every counter zero");
            }
            self.result.statuses += 1;
            self.result.final_seen = true;
            self.result.last_status = Some(Status { flags, counters: n });
            return Ok(());
        };
        if let Some(p) = &self.result.last_status {
            for (j, (&now, &was)) in n.iter().zip(p.counters.iter()).enumerate() {
                if j != 4 && j != 14 && now < was {
                    return bad(r, format!("AUTO_STATUS {} decreased", STATUS_FIELDS[j]));
                }
            }
            for sticky in [ST_SATURATED, ST_RESOURCE_CONFLICT, ST_CONTEXT_RESET, ST_SEGMENT_QUARANTINE, ST_FRAME_MARK_MISSED,
                ST_FRAMES_FLIP, ST_FRAMES_EXPLICIT, ST_SEGMENT_REPLAY] {
                if p.flags & sticky != 0 && flags & sticky == 0 {
                    return bad(r, format!("AUTO_STATUS sticky flag {sticky} cleared"));
                }
            }
        }
        let lower = flags & ST_LOWER_BOUND != 0;
        if flags & (ST_OWNER_UNACK | ST_SATURATED) != 0 && !lower {
            return bad(r, "AUTO_STATUS OWNER_UNACK or SATURATED without LOWER_BOUND");
        }
        if flags & ST_SATURATED == 0 {
            let sum = n[2].checked_add(n[3]).and_then(|x| x.checked_add(n[4]));
            match sum {
                Some(s) if s == n[1] || (lower && s <= n[1]) => {}
                _ => return bad(r, "AUTO_STATUS admitted is not written + dropped + pending"),
            }
        }
        if n[0] < self.last_op || n[13] < self.result.max_snapshot || n[13] > u64::from(u32::MAX) {
            return bad(r, "AUTO_STATUS frontier below the retained records");
        }
        if n[14] > u64::from(a.pairs) {
            return bad(r, format!("AUTO_STATUS holds {} report pairs of {}", n[14], a.pairs));
        }
        if n[10] > n[9] {
            return bad(r, "AUTO_STATUS more clock anchors written than attempted");
        }
        if fin {
            if n[2] != self.result.ops_parsed || n[5] != self.result.frames_parsed || n[10] != self.result.clocks_parsed {
                return bad(r, format!("FINAL AUTO_STATUS written {}/{}/{} against parsed {}/{}/{}", n[2], n[5],
                    n[10], self.result.ops_parsed, self.result.frames_parsed, self.result.clocks_parsed));
            }
            if !lower && (n[4] != 0 || n[14] != 0 || n[6] != 0 || n[9] - n[10] != n[11]) {
                return bad(r, "exact FINAL AUTO_STATUS with pending, held, lost frames or unaccounted anchors");
            }
            self.result.final_seen = true;
        }
        self.result.statuses += 1;
        self.result.last_status = Some(Status { flags, counters: n });
        Ok(())
    }

    fn clock(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::AutoClock;
        let cfg = self.attached().clone();
        if payload.len() != CLOCK_BYTES {
            return bad(r, format!("AUTO_CLOCK is {} bytes, not {CLOCK_BYTES}", payload.len()));
        }
        let mut c = Cursor::new(payload, "AUTO_CLOCK");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let epoch = c.u32()?;
        let id = c.u32()?;
        let role = c.u32()?;
        let slot = c.u32()?;
        let serial = c.u32()?;
        let polls = c.u32()?;
        let tb_before = c.u64()?;
        let tb_after = c.u64()?;
        let raw = c.u64()?;
        let repeated = c.u64()?;
        let label_first = c.u32()?;
        let label_second = c.u32()?;
        let frame = c.u64()?;
        let owner = c.u32()?;
        let r0 = c.u32()?;
        let elapsed = c.u64()?;
        c.done()?;
        let at = format!("AUTO clock anchor {id}");
        if schema != 1 || instance != cfg.instance || epoch != cfg.epoch || r0 != 0 || role != 0 {
            return bad(r, format!("{at}: schema {schema} instance {instance} epoch {epoch} role {role} or reserved"));
        }
        if ![CK_VALID, CK_TIMEOUT, CK_REGRESSION, CK_READ_REFUSED].contains(&flags) {
            return bad(r, format!("{at}: flags {flags:#x} (exactly one outcome)"));
        }
        // Slots are reused; anchor ids and serials never are (one nonwrapping per-instance counter).
        if id <= self.last_anchor || serial != id || id == u32::MAX {
            return bad(r, format!("{at}: id/serial {id}/{serial} not above {}", self.last_anchor));
        }
        if !(cfg.report_first..cfg.report_first + CLOCK_SLOTS).contains(&slot) {
            return bad(r, format!("{at}: slot {slot} outside the clock reports"));
        }
        if polls > CLOCK_POLLS || owner != cfg.owner {
            return bad(r, format!("{at}: polls {polls} owner {owner}"));
        }
        if flags == CK_VALID {
            if raw == u64::MAX || raw != repeated || label_first != serial || label_second != serial
                || tb_after < tb_before || elapsed != tb_after - tb_before || polls == 0
            {
                return bad(r, format!("{at}: VALID without a stable exact completion"));
            }
        } else if raw != 0 || repeated != 0 {
            return bad(r, format!("{at}: failed anchor with raw values"));
        }
        self.last_anchor = id;
        self.result.clocks_parsed += 1;
        if self.result.clocks.len() as u64 >= self.budgets.max_clocks {
            self.result.refusal.get_or_insert("AUTO clock retention ceiling");
            return Ok(());
        }
        self.result.clocks.push(Anchor {
            flags,
            epoch,
            id,
            role,
            slot,
            serial,
            polls,
            tb_before,
            tb_after,
            raw,
            repeated,
            label_first,
            label_second,
            frame,
            owner,
            elapsed,
        });
        Ok(())
    }
}

fn rule_of(ty: u32) -> Rule {
    match ty {
        CHUNK_AUTO_CONFIG => Rule::AutoConfig,
        CHUNK_AUTO_OPS => Rule::AutoOps,
        CHUNK_AUTO_COVERAGE => Rule::AutoCoverage,
        CHUNK_AUTO_STATUS => Rule::AutoStatus,
        _ => Rule::AutoClock,
    }
}

// ---- the capture's own clock map ---------------------------------------------------------------------------------

/// The PPU-ticks-per-RSX-tick slope interval of a qualified map, and its interpolation support.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Map {
    pub anchors: u64,
    pub fmin: i128,
    pub fmax: i128,
    pub slope_lo: Rat,
    pub slope_hi: Rat,
    pub decisive: (u64, u64),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MapState {
    Qualified(Box<Map>),
    /// The exact missing condition.
    Unqualified(String),
}

/// Qualification (stage 2 target plan, section 6): every anchor VALID, at least four distinct RSX coordinates, a
/// feasible bounded positive slope, and in each H-SPLIT parity at least one DECISIVE held-out anchor with zero
/// contradictions.  No extrapolation: conversions are allowed only inside [fmin, fmax].
pub fn qualify(inv: &Inventory) -> MapState {
    let un = |s: String| MapState::Unqualified(s);
    if inv.config.as_ref().and_then(Config::attached).is_none() {
        return un("no attached AUTO configuration".into());
    }
    if inv.refusal.is_some() || inv.clocks_parsed != inv.clocks.len() as u64 {
        return un("clock anchors not all retained".into());
    }
    if let Some(a) = inv.clocks.iter().find(|a| a.flags != CK_VALID) {
        return un(format!("anchor {} failed ({})", a.id, a.outcome()));
    }
    if let Some(s) = &inv.last_status {
        if s.counters[11] != 0 {
            return un(format!("{} anchor(s) lost to capacity", s.counters[11]));
        }
    }
    let obs: Vec<Obs> = inv
        .clocks
        .iter()
        .map(|a| Obs { f: i128::from(a.raw), lo: i128::from(a.tb_before), hi: i128::from(a.tb_after) })
        .collect();
    let mut distinct: Vec<i128> = obs.iter().map(|o| o.f).collect();
    distinct.sort_unstable();
    distinct.dedup();
    if distinct.len() < 4 {
        return un(format!("{} distinct anchor coordinate(s), fewer than 4", distinct.len()));
    }
    let n = obs.len() as u64;
    let mut m = Meter::new(
        clockmap::solve_budget(n).saturating_add(split_budget(n).saturating_mul(2)),
        Limit::Capture,
    );
    let model = match clockmap::solve(&mut m, &obs) {
        Ok(Solved::Compatible(model)) => model,
        Ok(Solved::Inconsistent) => return un("anchors inconsistent with one affine map".into()),
        Ok(Solved::NoObservations) => return un("no anchors".into()),
        Err(l) => return un(format!("map refused: {}", l.name())),
    };
    let Some(hi) = model.slope_hi.clone() else { return un("slope unbounded".into()) };
    if model.slope_lo_open() {
        return un("slope not bounded away from zero".into());
    }
    let mut decisive = [0u64; 2];
    for (k, even) in [false, true].into_iter().enumerate() {
        match split_holdout(&mut m, &obs, even) {
            Ok(SplitOutcome::Checked { held, .. }) => {
                if let Some(h) = held.iter().find(|h| h.contradicted) {
                    return un(format!("H-SPLIT {} hold-out anchor ordinal {} contradicted",
                        if even { "even" } else { "odd" }, h.ordinal));
                }
                decisive[k] = held.iter().filter(|h| h.decisive).count() as u64;
                if decisive[k] == 0 {
                    return un(format!("H-SPLIT {} parity has no decisive hold-out", if even { "even" } else { "odd" }));
                }
            }
            Ok(SplitOutcome::NotAvailable(why)) => return un(format!("H-SPLIT: {why}")),
            Err(l) => return un(format!("H-SPLIT refused: {}", l.name())),
        }
    }
    MapState::Qualified(Box::new(Map {
        anchors: n,
        fmin: model.fmin,
        fmax: model.fmax,
        slope_lo: model.slope_lo.clone(),
        slope_hi: hi,
        decisive: (decisive[0], decisive[1]),
    }))
}

fn floor_mul(x: u64, r: &Rat) -> BigInt {
    let v = BigInt::from(x) * &r.num;
    let q = &v / &r.den;
    if v.is_negative() && !(&v % &r.den).is_zero() { q - 1 } else { q }
}
fn ceil_mul(x: u64, r: &Rat) -> BigInt {
    let v = BigInt::from(x) * &r.num;
    let q = &v / &r.den;
    if !v.is_negative() && !(&v % &r.den).is_zero() { q + 1 } else { q }
}
fn sat(v: &BigInt) -> u64 {
    v.to_u64().unwrap_or(if v.is_negative() { 0 } else { u64::MAX })
}

// ---- per-frame reading -------------------------------------------------------------------------------------------

/// A share threshold num/den: `part * den >= whole * num`.
pub const WAIT_SHARE: (u64, u64) = (1, 4);
pub const RSX_SPAN_SHARE: (u64, u64) = (1, 2);
/// Presentation (flip-status) wait share of the PPU frame interval that, with the RSX not limiting, reads as pacing.
pub const PACING_SHARE: (u64, u64) = (1, 4);
pub const NOT_DISTINGUISHED: &str = "RSX not limiting; PPU work vs presentation pacing not distinguished";
/// Flip-status polling was seen, but polling shows how often the owner looked at the flip, not that it waited.
pub const POLLING_ONLY: &str = "presentation polling observed; waiting not established";
/// A frame with CALL-segment records: no per-frame verdict (the records do not carry the building frame).
pub const SEGMENT_POPULATION: &str = "from CALL segments: a segment is counted where it was built and recorded where \
    it was called, so this frame's records and coverage are not one population";
/// 63/11 STATUS SEGMENT_REPLAY: no frame of the capture has a verdict.
pub const SEGMENT_REPLAY: &str = "a CALL segment was replayed without a rebuild: its report writes may have overwritten \
    other records' timestamps";
/// A flip-wait scope crossed a boundary of this frame: its time belongs to no single frame and is charged to neither.
pub const WAIT_SPANS: &str = "presentation attribution unavailable: a flip-wait scope spans a frame boundary";
/// A polling-cadence run crossed a boundary of this frame (charged to neither frame).
pub const POLLING_SPANS: &str = "presentation attribution unavailable: flip-status polling spans a frame boundary";
pub const FINAL_FRAME: &str = "final frame closed at shutdown (no end mark)";
/// A frame's counts summed across kinds (observed or excluded operations) do not fit in u64: no verdict is formed.
pub const TOTALS_OVERFLOW: &str = "coverage totals overflow";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    PpuBound,
    RsxBound,
    Mixed,
    Pacing,
    Unknown,
}
impl Verdict {
    pub fn label(self) -> &'static str {
        match self {
            Verdict::PpuBound => "PPU-bound candidate",
            Verdict::RsxBound => "RSX-bound candidate",
            Verdict::Mixed => "mixed candidate",
            Verdict::Pacing => "pacing",
            Verdict::Unknown => "UNKNOWN",
        }
    }
    pub fn key(self) -> &'static str {
        match self {
            Verdict::PpuBound => "PPU_BOUND",
            Verdict::RsxBound => "RSX_BOUND",
            Verdict::Mixed => "MIXED",
            Verdict::Pacing => "PACING",
            Verdict::Unknown => "UNKNOWN",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameView {
    pub owner: u32,
    pub frame: u64,
    pub flags: u16,
    pub tb_begin: u64,
    pub tb_end: u64,
    pub visited: [u64; 3],
    pub observed: [u64; 3],
    pub excluded: [u64; 3],
    /// Retained operation records of this frame by kind (ARRAY, INDEX, CLEAR).
    pub records: [u64; 3],
    pub unsupported: u64,
    pub ops_valid: u64,
    pub ops_incomplete: u64,
    /// RSX reached-marker span of the frame's operations (first begin to last end), raw ticks.
    pub rsx_span_raw: Option<u64>,
    /// The same span on the PPU clock through the qualified map: [lo, hi] ticks.
    pub rsx_span_ppu: Option<(u64, u64)>,
    /// PPU ticks the owner provably waited in complete FIFO wrap phases tagged with this frame (lower bound).
    pub fifo_wait: Option<u64>,
    /// D2: the raw RSX timestamps of the frame's begin and end marks (raw-valid marks only).
    pub rsx_frame_raw: (Option<u64>, Option<u64>),
    /// D2: end mark minus begin mark on the PPU clock through the qualified map: [lo, hi] ticks.
    pub rsx_frame_ppu: Option<(u64, u64)>,
    /// Covered operations recorded for this frame that were built inside CALL segments.
    pub segment_ops: u64,
    /// 63/11 only (else zero): `records` is then the frame's BUILD population (records not built in a segment
    /// recorded here, plus segment records built here wherever recorded), reconciled with `observed`; `recorded` is
    /// the EXECUTION population (covered records recorded here, which timing uses); `segment_imports` the segment
    /// records recorded here but built in another frame; `segment_unrepresented` the row's u32@380;
    /// `segment_abandoned` segment draws built here whose records were dropped without a CALL.
    pub recorded: u64,
    pub segment_imports: u64,
    pub segment_unrepresented: u32,
    pub segment_abandoned: u64,
    /// The frame's presentation evidence (FR_PRESENT rows), None when the writer observed none.
    pub presentation: Option<Presentation>,
    pub verdict: Verdict,
    pub reason: String,
}

/// Presentation evidence of one frame.  Only `flip_wait` (flip-wait scope elapsed time, including scheduling and
/// polling) is waiting evidence; cadence and the 15/11 poll intervals are observations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Presentation {
    /// Flip-wait scope ticks and scope count, with timed-out and boundary-spanning scopes (31/11 writer only).
    pub flip_wait: Option<(u64, u32)>,
    pub flip_wait_timeouts: u32,
    pub flip_wait_spanning: u32,
    pub cadence: Option<Cadence>,
    /// 15/11 writer: poll intervals (first pending to done poll) and their ticks; never waiting evidence.
    pub legacy_polls: Option<(u32, u64)>,
}

#[derive(Debug, Clone, Default)]
pub struct Report {
    pub capability: bool,
    /// The capture's CONFIG is 63/11 (segment records carry their build frame).
    pub build_frames: bool,
    pub state: String,
    pub config: Option<Config>,
    pub status: Option<Status>,
    pub map: Option<MapState>,
    pub ops_parsed: u64,
    pub ops_valid: u64,
    pub clocks: Vec<Anchor>,
    pub frames: Vec<FrameView>,
    pub refusal: Option<&'static str>,
}

/// The FIFO wait evidence of one frame: Ok(lower-bound ticks) or the reason it is unavailable.
fn fifo_wait(cap: &Capture, fifo: &crate::fifo::Report, owner: u32, frame: u64) -> Result<u64, String> {
    use crate::fifo::Phase;
    if cap.fifo.is_none() {
        return Err("no FIFO wait evidence in this capture".into());
    }
    if fifo.refusal.is_some() || !fifo.final_seen || fifo.status.as_ref().is_some_and(|s| !s.exact()) {
        return Err(format!("FIFO wait evidence incomplete ({})", fifo.coverage));
    }
    let mut total = 0u64;
    for w in fifo.wraps.iter().filter(|w| w.token == Some((owner, frame))) {
        match &w.outer {
            Phase::Complete { min, .. } => total = total.saturating_add(*min),
            _ => return Err(format!("FIFO wrap {} of this frame is {}", w.wrap_id, w.outer.label())),
        }
    }
    Ok(total)
}

fn share(part: u64, whole: u64, (num, den): (u64, u64)) -> bool {
    u128::from(part) * u128::from(den) >= u128::from(whole) * u128::from(num)
}

/// The BUILD population of every frame of a 63/11 capture by kind: records not built in a segment under their own
/// token, plus segment records under their build frame wherever (and whether) they were recorded.  Also the dropped
/// segment draws (call ordinal 0) per build frame.
fn build_populations(inv: &Inventory) -> (BTreeMap<(u32, u64), [u64; 3]>, BTreeMap<(u32, u64), u64>) {
    let (mut built, mut abandoned) = (BTreeMap::new(), BTreeMap::new());
    for o in inv.ops.iter().filter(|o| o.covered()) {
        let key = if o.flags & OP_SEGMENT != 0 { o.build } else { o.token() };
        let Some(key) = key else { continue };
        let row: &mut [u64; 3] = built.entry(key).or_default();
        row[usize::from(o.kind - KIND_ARRAY)] += 1;
        if o.flags & OP_SEGMENT != 0 && o.call == 0 {
            *abandoned.entry(key).or_default() += 1;
        }
    }
    (built, abandoned)
}

/// A frame row is a clean BUILD source for segment draws recorded elsewhere: present, associated, closed exactly,
/// nothing excluded or outside the observed population, and its build population reconciles kind by kind.  This does
/// not depend on that frame's own verdict (its own imports, timing or presentation).
fn build_clean(f: &Coverage, built: &BTreeMap<(u32, u64), [u64; 3]>) -> Result<(), String> {
    if f.flags & FR_TOKEN == 0 {
        return Err("unassociated coverage row".into());
    }
    if f.flags & (FR_OPEN | FR_SATURATED | FR_LOSS) != 0 {
        return Err("coverage row OPEN, SATURATED or after coverage loss".into());
    }
    match (0..3).try_fold(0u64, |a, k| a.checked_add(f.excluded_checked(k)?)) {
        None => return Err(TOTALS_OVERFLOW.into()),
        Some(0) => {}
        Some(n) => return Err(format!("{n} covered operation(s) excluded")),
    }
    if f.unsupported != 0 {
        return Err(format!("{} visit(s) outside the observed population (segment or private buffer)", f.unsupported));
    }
    let records = built.get(&(f.owner, f.frame)).copied().unwrap_or_default();
    if let Some(k) = (0..3).find(|&k| records[k] != f.rows[k][1]) {
        return Err(format!("{} {} operation record(s) for {} observed", records[k], KINDS[k], f.rows[k][1]));
    }
    Ok(())
}

pub fn analyze(cap: &Capture) -> Report {
    let Some(inv) = cap.auto.as_ref() else {
        return Report { state: "UNSUPPORTED".into(), ..Report::default() };
    };
    let mut rep = Report {
        capability: true,
        state: inv.state(),
        config: inv.config.clone(),
        status: inv.last_status.clone(),
        ops_parsed: inv.ops_parsed,
        ops_valid: inv.ops.iter().filter(|o| o.raw_valid()).count() as u64,
        clocks: inv.clocks.clone(),
        refusal: inv.refusal,
        ..Report::default()
    };
    let Some(Config::Attached(attached)) = &inv.config else { return rep };
    let build = attached.build_frames();
    rep.build_frames = build;
    let (built, abandoned) = if build { build_populations(inv) } else { Default::default() };
    let rows: BTreeMap<(u32, u64), &Coverage> = inv.frames.iter().map(|f| ((f.owner, f.frame), f)).collect();
    let map = qualify(inv);
    let fifo = crate::fifo::analyze(cap);
    let status_ok = inv.final_seen && inv.refusal.is_none();
    let status_flags = inv.last_status.as_ref().map_or(0, |s| s.flags);
    for f in &inv.frames {
        let mine = |o: &&Op| o.token() == Some((f.owner, f.frame));
        let ops: Vec<&Op> = inv.ops.iter().filter(mine).filter(|o| o.covered()).collect();
        let mark = |role: u32| inv.ops.iter().filter(mine)
            .find(|o| o.kind == KIND_FRAME && o.args[0] == role && o.raw_valid() && o.flags & OP_CLOCK_INVALID == 0);
        let rsx_frame_raw = (mark(FRAME_BEGIN).map(|o| o.raw_begin), mark(FRAME_END).map(|o| o.raw_end));
        let mut v = FrameView {
            owner: f.owner,
            frame: f.frame,
            flags: f.flags,
            tb_begin: f.tb_begin,
            tb_end: f.tb_end,
            visited: [f.rows[0][0], f.rows[1][0], f.rows[2][0]],
            observed: [f.rows[0][1], f.rows[1][1], f.rows[2][1]],
            excluded: [f.excluded(0), f.excluded(1), f.excluded(2)],
            unsupported: f.unsupported,
            records: if build {
                built.get(&(f.owner, f.frame)).copied().unwrap_or_default()
            } else {
                [KIND_ARRAY, KIND_INDEX, KIND_CLEAR].map(|k| ops.iter().filter(|o| o.kind == k).count() as u64)
            },
            ops_valid: ops.iter().filter(|o| o.raw_valid()).count() as u64,
            ops_incomplete: ops.iter().filter(|o| !o.raw_valid()).count() as u64,
            rsx_span_raw: None,
            rsx_span_ppu: None,
            fifo_wait: None,
            rsx_frame_raw,
            rsx_frame_ppu: None,
            segment_ops: ops.iter().filter(|o| o.flags & OP_SEGMENT != 0).count() as u64,
            recorded: if build { ops.len() as u64 } else { 0 },
            segment_imports: if build {
                ops.iter().filter(|o| o.flags & OP_SEGMENT != 0 && o.build != Some((f.owner, f.frame))).count() as u64
            } else {
                0
            },
            segment_unrepresented: f.segment_unrepresented,
            segment_abandoned: abandoned.get(&(f.owner, f.frame)).copied().unwrap_or(0),
            presentation: (f.flags & FR_PRESENT != 0).then(|| Presentation {
                flip_wait: f.legacy_polls.is_none().then_some((f.flip_wait, f.flip_waits)),
                flip_wait_timeouts: f.flip_wait_timeouts,
                flip_wait_spanning: f.flip_wait_spanning,
                cadence: f.legacy_polls.is_none().then_some(f.cadence),
                legacy_polls: f.legacy_polls,
            }),
            verdict: Verdict::Unknown,
            reason: String::new(),
        };
        // Checked aggregation: per-kind rows are each valid on the wire, but their cross-kind totals may not fit.
        let observed: Option<u64> = v.observed.iter().try_fold(0u64, |a, &b| a.checked_add(b));
        let excluded: Option<u64> = (0..3).try_fold(0u64, |a, k| a.checked_add(f.excluded_checked(k)?));
        if !ops.is_empty() && ops.iter().all(|o| o.raw_valid() && o.flags & OP_CLOCK_INVALID == 0) {
            let lo = ops.iter().map(|o| o.raw_begin).min().unwrap();
            let hi = ops.iter().map(|o| o.raw_end).max().unwrap();
            v.rsx_span_raw = Some(hi - lo);
            if let MapState::Qualified(m) = &map {
                if i128::from(lo) >= m.fmin && i128::from(hi) <= m.fmax {
                    v.rsx_span_ppu = Some((sat(&floor_mul(hi - lo, &m.slope_lo)), sat(&ceil_mul(hi - lo, &m.slope_hi))));
                }
            }
        }
        if let (MapState::Qualified(m), (Some(b), Some(e))) = (&map, rsx_frame_raw) {
            if b <= e && i128::from(b) >= m.fmin && i128::from(e) <= m.fmax {
                v.rsx_frame_ppu = Some((sat(&floor_mul(e - b, &m.slope_lo)), sat(&ceil_mul(e - b, &m.slope_hi))));
            }
        }
        let wait = fifo_wait(cap, &fifo, f.owner, f.frame);
        v.fifo_wait = wait.as_ref().ok().copied();
        let d = f.tb_end.saturating_sub(f.tb_begin);
        let last = inv.frames.last().is_some_and(|l| std::ptr::eq(l, f));
        let present = v.presentation.clone();
        v.reason = (|| -> Result<(Verdict, String), String> {
            if !status_ok {
                return Err(format!("capture not final ({})", inv.state()));
            }
            if status_flags & (ST_RESOURCE_CONFLICT | ST_CONTEXT_RESET) != 0 {
                return Err("timing invalidated by a resource conflict or context reset".into());
            }
            if status_flags & ST_SEGMENT_REPLAY != 0 {
                return Err(SEGMENT_REPLAY.into());
            }
            if f.flags & FR_TOKEN == 0 {
                return Err("unassociated coverage row".into());
            }
            if last && inv.final_seen && f.flags & FR_OPEN != 0 && v.rsx_frame_raw.1.is_none() {
                return Err(FINAL_FRAME.into());
            }
            if f.flags & (FR_OPEN | FR_SATURATED | FR_LOSS) != 0 {
                return Err("coverage row OPEN, SATURATED or after coverage loss".into());
            }
            let (Some(_), Some(excluded)) = (observed, excluded) else {
                return Err(TOTALS_OVERFLOW.into());
            };
            if excluded != 0 {
                return Err(format!("{excluded} covered operation(s) excluded"));
            }
            if f.unsupported != 0 {
                return Err(format!("{} visit(s) outside the observed population (segment or private buffer)",
                    f.unsupported));
            }
            // A segment's operations are counted in the row of the frame that BUILT it, but recorded under the frame
            // that CALLED it.  Before 63/11 a record does not say where it was built, so a frame with segment records
            // cannot be reconciled against its row: equal counts may describe different segments (one built here,
            // another called here), and an exclusion in the building frame would be lost.
            if !build && v.segment_ops != 0 {
                return Err(format!("{} operation record(s) {SEGMENT_POPULATION}", v.segment_ops));
            }
            // 63/11: every segment draw a CALL executed here has a record carrying this frame's token.
            if v.segment_unrepresented != 0 {
                return Err(format!("at least {} segment draw(s) executed in this frame have no record (a CALL of an \
                    untracked, replayed, refused or partly untimed segment)", v.segment_unrepresented));
            }
            // Reconcile the row's population with its records kind by kind: equal totals over different kinds are
            // not the population the row describes.  With 63/11 that population is what this frame BUILT.
            if let Some(k) = (0..3).find(|&k| v.records[k] != v.observed[k]) {
                return Err(format!("{} {} operation record(s) for {} observed", v.records[k], KINDS[k],
                    v.observed[k]));
            }
            if v.segment_abandoned != 0 {
                return Err(format!("{} segment draw(s) built in this frame were dropped (never called, or their \
                    CALL refused)", v.segment_abandoned));
            }
            // Segment draws executed here but built elsewhere: each build frame must be a clean, reconciled build
            // source (its row, not its own verdict).
            if build {
                let mut sources = BTreeSet::new();
                for o in ops.iter().filter(|o| o.flags & OP_SEGMENT != 0) {
                    match o.build {
                        None => return Err("segment record(s) built while no coverage frame was open".into()),
                        Some(g) if g != (f.owner, f.frame) => {
                            sources.insert(g);
                        }
                        Some(_) => {}
                    }
                }
                for (o, g) in sources {
                    let Some(row) = rows.get(&(o, g)) else {
                        return Err(format!("segment draw(s) built in frame {o}/{g}, which has no coverage row"));
                    };
                    build_clean(row, &built).map_err(|why| format!("segment draw(s) built in frame {o}/{g}: {why}"))?;
                }
            }
            if v.ops_incomplete != 0 {
                return Err(format!("{} operation(s) without raw-valid timestamps", v.ops_incomplete));
            }
            let w = wait.clone()?;
            if let MapState::Unqualified(why) = &map {
                return Err(format!("clock map unqualified: {why}"));
            }
            if d == 0 {
                return Err("zero-length PPU frame interval".into());
            }
            // The EXECUTION population (records recorded here) decides emptiness and span; with 63/11 the row's
            // observed counts are what this frame built, which a CALL-only or build-only frame does not match.
            let span = match (ops.is_empty(), v.rsx_span_ppu) {
                (true, _) => (0, 0),
                (false, Some(s)) => s,
                (false, None) => return Err("RSX span outside the map's interpolation support".into()),
            };
            let rsx_long = share(span.0, d, RSX_SPAN_SHARE);
            let rsx_short = !share(span.1, d, RSX_SPAN_SHARE);
            let waited = share(w, d, WAIT_SHARE);
            // PPU-bound needs positive evidence that the owner was busy, which this family does not carry: a frame
            // where the RSX is not limiting is PACING only with flip-wait scope evidence (cellGcmWaitFlipComplete),
            // otherwise UNKNOWN.  Polling cadence and 15/11 poll intervals are named observations, never waiting.
            if waited && rsx_long {
                Ok((Verdict::RsxBound, format!("fetch: owner waited >= {w} of {d} PPU ticks in FIFO wraps; RSX \
                    reached-marker span >= {} ticks", span.0)))
            } else if w == 0 && rsx_short {
                Err(match present {
                    Some(Presentation { flip_wait: Some((p, n)), .. }) if share(p, d, PACING_SHARE) => {
                        return Ok((Verdict::Pacing, format!("presentation: flip-wait scope elapsed time, including \
                            scheduling and polling, >= {p} of {d} PPU ticks ({n} scope(s)); no FIFO wait; RSX \
                            reached-marker span <= {} ticks", span.1)));
                    }
                    // Boundary-spanning evidence belongs to no frame: without a complete scope of its own that
                    // reaches the share, the frame's presentation attribution is unavailable.
                    Some(Presentation { flip_wait_spanning: s, .. }) if s > 0 => WAIT_SPANS.into(),
                    Some(Presentation { cadence: Some(c), .. }) if c.spanning > 0 => POLLING_SPANS.into(),
                    Some(Presentation { legacy_polls: Some((n, _)), .. }) if n > 0 => {
                        format!("{POLLING_ONLY} (writer without flip-wait scopes)")
                    }
                    Some(Presentation { cadence: Some(c), .. }) if c.runs > 0 || c.broken > 0 => POLLING_ONLY.into(),
                    _ => NOT_DISTINGUISHED.into(),
                })
            } else if w > 0 && !waited && rsx_long {
                Ok((Verdict::Mixed, format!("owner waited >= {w} of {d} PPU ticks; RSX reached-marker span >= {} \
                    ticks", span.0)))
            } else {
                Err(format!("unresolved: FIFO wait >= {w}, RSX span {}..{} of {d} PPU ticks", span.0, span.1))
            }
        })()
        .map(|(verdict, why)| {
            v.verdict = verdict;
            why
        })
        .unwrap_or_else(|why| why);
        rep.frames.push(v);
    }
    rep.map = Some(map);
    rep
}

pub const SCOPE: &str = "Reached-marker intervals between two RSX command-stream positions around covered draws and \
clears, including dependencies and pipeline effects between them: not isolated draw cost, not utilization, not GPU \
busy time. Frame tokens identify submission, not GPU execution containment.";

/// What delimited the capture's frames, from STATUS (FLIP, EXPLICIT, both, or none seen).
pub fn frame_source(r: &Report) -> &'static str {
    match r.status.as_ref().map(|s| s.flags & (ST_FRAMES_FLIP | ST_FRAMES_EXPLICIT)) {
        Some(f) if f == ST_FRAMES_FLIP | ST_FRAMES_EXPLICIT => "MIXED (flips until the first explicit ps3dk_prof_frame)",
        Some(ST_FRAMES_FLIP) => "FLIP",
        Some(ST_FRAMES_EXPLICIT) => "EXPLICIT",
        _ => "NONE (no flip or explicit frame seen: operations unassociated)",
    }
}

pub const INTERVAL_NOTE: &str = "An operation interval runs from the RSX reaching its begin marker to reaching its end marker, so it includes any work queued ahead of the operation that the RSX was still finishing: it is not the isolated cost of that draw or clear.";

pub fn summary_line(inv: &Inventory) -> String {
    format!("Automatic RSX: {}; {} operation(s), {} frame row(s), {} clock anchor(s); use auto", inv.state(),
        inv.ops_parsed, inv.frames_parsed, inv.clocks_parsed)
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn rat_text(r: &Rat) -> String {
    format!("{}/{}", r.num, r.den)
}

pub fn write_text<W: std::io::Write>(out: &mut W, r: &Report) -> std::io::Result<()> {
    writeln!(out, "Automatic RSX observation: {}", r.state)?;
    writeln!(out, "{SCOPE}")?;
    if !r.capability {
        return Ok(());
    }
    if let Some(why) = r.refusal {
        writeln!(out, "Refused: {why}; per-frame verdicts are UNKNOWN.")?;
    }
    match &r.config {
        Some(Config::Refused { reason, application_rc, .. }) => {
            writeln!(out, "automatic observation unavailable: {} (reason {reason}, GCM return code {application_rc})",
                reason_name(*reason))?;
            writeln!(out, "This is unavailable observation, not a frame with zero RSX work.")?;
            return Ok(());
        }
        Some(Config::Attached(a)) => writeln!(out, "instance {} domain {} epoch {} owner {} generation {}; reports \
            {}+{} ({} pairs), labels {}/{}; metadata {} frames {}; allocated {} of {} bytes; mechanism {} binding {}",
            a.instance, a.domain, a.epoch, a.owner, a.generation, a.report_first, a.report_count, a.pairs, a.op_label,
            a.clock_label, a.metadata, a.frame_capacity, a.allocated, a.ceiling, hex(&a.mechanism), hex(&a.binding))?,
        None => writeln!(out, "no AUTO_CONFIG")?,
    }
    if let Some(s) = &r.status {
        let fields: Vec<String> = STATUS_FIELDS.iter().zip(s.counters.iter()).map(|(n, v)| format!("{n} {v}")).collect();
        writeln!(out, "status flags {:#x}: {}", s.flags, fields.join(", "))?;
    }
    if r.status.as_ref().is_some_and(|s| s.flags & ST_FRAME_MARK_MISSED != 0) {
        writeln!(out, "FRAME marks missed: a frame without both marks has no RSX frame duration (UNKNOWN, not zero)")?;
    }
    if r.status.as_ref().is_some_and(|s| s.flags & ST_SEGMENT_REPLAY != 0) {
        writeln!(out, "SEGMENT REPLAY: {SEGMENT_REPLAY}; every frame is UNKNOWN")?;
    }
    writeln!(out, "frames delimited by: {}", frame_source(r))?;
    writeln!(out, "operations: {} parsed, {} raw-valid", r.ops_parsed, r.ops_valid)?;
    writeln!(out, "note: {INTERVAL_NOTE}")?;
    let failed = r.clocks.iter().filter(|a| a.flags != CK_VALID).count();
    writeln!(out, "clock anchors: {} ({} failed)", r.clocks.len(), failed)?;
    match &r.map {
        Some(MapState::Qualified(m)) => writeln!(out, "clock map: capture mapping qualified (conditional); {} anchors, \
            PPU ticks per RSX tick in [{}, {}], support raw {}..{}, decisive hold-outs odd {} even {}", m.anchors,
            rat_text(&m.slope_lo), rat_text(&m.slope_hi), m.fmin, m.fmax, m.decisive.0, m.decisive.1)?,
        Some(MapState::Unqualified(why)) => writeln!(out, "clock map: AUTO_CLOCK_UNQUALIFIED ({why}); raw RSX ticks only")?,
        None => {}
    }
    for f in &r.frames {
        writeln!(out, "frame {}/{}: {} - {}; ppu {} ticks; visited {:?} observed {:?} excluded {:?} unsupported {}; \
            ops {} valid {} incomplete ({} in segments); rsx span raw {} ppu {}; rsx frame marks {}..{} ppu {}; \
            presentation {}; fifo wait {}",
            f.owner, f.frame, f.verdict.label(), f.reason, f.tb_end.saturating_sub(f.tb_begin), f.visited, f.observed,
            f.excluded, f.unsupported, f.ops_valid, f.ops_incomplete, f.segment_ops,
            f.rsx_span_raw.map_or("UNKNOWN".into(), |x| x.to_string()),
            f.rsx_span_ppu.map_or("UNKNOWN".into(), |(a, b)| format!("{a}..{b}")),
            f.rsx_frame_raw.0.map_or("UNKNOWN".into(), |x| x.to_string()),
            f.rsx_frame_raw.1.map_or("UNKNOWN".into(), |x| x.to_string()),
            f.rsx_frame_ppu.map_or("UNKNOWN".into(), |(a, b)| format!("{a}..{b}")),
            f.presentation.as_ref().map_or("not observed".into(), presentation_text),
            f.fifo_wait.map_or("UNKNOWN".into(), |x| format!(">= {x}")))?;
        if r.build_frames {
            writeln!(out, "  segments: built records {:?} recorded here {}; imports {}, unrepresented >= {}, dropped \
                builds {}", f.records, f.recorded, f.segment_imports, f.segment_unrepresented, f.segment_abandoned)?;
        }
    }
    Ok(())
}

fn presentation_text(p: &Presentation) -> String {
    let mut parts = Vec::new();
    if let Some((t, n)) = p.flip_wait {
        parts.push(format!("flip-wait scopes {n} ({t} ticks elapsed, including scheduling and polling; {} timed out, \
            {} across a frame boundary)", p.flip_wait_timeouts, p.flip_wait_spanning));
    }
    if let Some(c) = p.cadence {
        parts.push(format!("polling cadence {} run(s) ({} ticks of bounded gaps), {} broken, {} across a frame \
            boundary", c.runs, c.ticks, c.broken, c.spanning));
    }
    if let Some((n, t)) = p.legacy_polls {
        parts.push(format!("flip-status poll intervals {n} ({t} ticks; observation only)"));
    }
    parts.join("; ")
}

fn presentation_json(p: &Presentation) -> serde_json::Value {
    use serde_json::json;
    json!({
        "flip_wait": p.flip_wait.map(|(t, n)| json!({"ticks":t,"scopes":n,"timeouts":p.flip_wait_timeouts,
            "spanning":p.flip_wait_spanning,
            "scope":"flip-wait scope elapsed time, including scheduling and polling"})),
        "polling_cadence": p.cadence.map(|c| json!({"runs":c.runs,"ticks":c.ticks,"broken":c.broken,
            "spanning":c.spanning})),
        "poll_intervals": p.legacy_polls.map(|(n, t)| json!({"count":n,"ticks":t})),
    })
}

pub fn write_json<W: std::io::Write>(out: &mut W, r: &Report) -> std::io::Result<()> {
    use serde_json::json;
    let config = match &r.config {
        Some(Config::Refused { instance, reason, application_rc, frequency, mechanism, binding }) => json!({
            "variant":"REFUSED","instance":instance,"reason":reason,"reason_name":reason_name(*reason),
            "application_rc":application_rc,"frequency":frequency,"mechanism":hex(mechanism),"binding":hex(binding)}),
        Some(Config::Attached(a)) => json!({"variant":"ATTACHED","instance":a.instance,"domain":a.domain,
            "epoch":a.epoch,"owner":a.owner,"generation":a.generation,"report_first":a.report_first,
            "report_count":a.report_count,"operation_label":a.op_label,"clock_label":a.clock_label,"pairs":a.pairs,
            "metadata":a.metadata,"frames":a.frame_capacity,"frequency":a.frequency,"allocated":a.allocated,
            "ceiling":a.ceiling,"mechanism":hex(&a.mechanism),"binding":hex(&a.binding)}),
        None => serde_json::Value::Null,
    };
    let status = r.status.as_ref().map(|s| {
        let mut o = serde_json::Map::new();
        o.insert("flags".into(), json!(s.flags));
        for (n, v) in STATUS_FIELDS.iter().zip(s.counters.iter()) {
            o.insert((*n).into(), json!(v));
        }
        serde_json::Value::Object(o)
    });
    let map = match &r.map {
        Some(MapState::Qualified(m)) => json!({"state":"QUALIFIED","anchors":m.anchors,
            "slope_lo":rat_text(&m.slope_lo),"slope_hi":rat_text(&m.slope_hi),"fmin":m.fmin.to_string(),
            "fmax":m.fmax.to_string(),"decisive_odd":m.decisive.0,"decisive_even":m.decisive.1}),
        Some(MapState::Unqualified(why)) => json!({"state":"UNQUALIFIED","reason":why}),
        None => serde_json::Value::Null,
    };
    let frames: Vec<serde_json::Value> = r.frames.iter().map(|f| (f, json!({
        "owner":f.owner,"frame":f.frame,"flags":f.flags,"tb_begin":f.tb_begin,"tb_end":f.tb_end,
        "visited":f.visited,"observed":f.observed,"excluded":f.excluded,"records":f.records,"unsupported":f.unsupported,
        "ops_valid":f.ops_valid,"ops_incomplete":f.ops_incomplete,"rsx_span_raw":f.rsx_span_raw,
        "rsx_span_ppu":f.rsx_span_ppu.map(|(a, b)| json!({"lo":a,"hi":b})),"fifo_wait_min":f.fifo_wait,
        "rsx_frame_begin_raw":f.rsx_frame_raw.0,"rsx_frame_end_raw":f.rsx_frame_raw.1,
        "rsx_frame_ppu":f.rsx_frame_ppu.map(|(a, b)| json!({"lo":a,"hi":b})),"segment_ops":f.segment_ops,
        "presentation":f.presentation.as_ref().map(presentation_json),
        "verdict":f.verdict.key(),"reason":f.reason}))).map(|(f, mut v)| {
        if r.build_frames {
            // 63/11: the build count (observed, reconciled with `records`) and the execution count are distinct
            let built: u64 = f.observed.iter().fold(0u64, |a, &b| a.saturating_add(b));
            v["built"] = json!(built);
            v["recorded"] = json!(f.recorded);
            v["segment_imports"] = json!(f.segment_imports);
            v["segment_unrepresented"] = json!(f.segment_unrepresented);
            v["segment_abandoned"] = json!(f.segment_abandoned);
        }
        v
    }).collect();
    let clocks: Vec<serde_json::Value> = r.clocks.iter().map(|a| json!({"id":a.id,"slot":a.slot,
        "outcome":a.outcome(),"frame":a.frame,"tb_before":a.tb_before,"tb_after":a.tb_after,"raw":a.raw,
        "polls":a.polls})).collect();
    let mut v = json!({
        "schema": 1,
        "capability": r.capability,
        "state": r.state,
        "refusal": r.refusal,
        "scope": SCOPE,
        "config": config,
        "status": status,
        "operations": {"parsed": r.ops_parsed, "raw_valid": r.ops_valid},
        "frame_marks_missed": r.status.as_ref().is_some_and(|s| s.flags & ST_FRAME_MARK_MISSED != 0),
        "frame_source": frame_source(r),
        "interval_note": INTERVAL_NOTE,
        "clock_map": map,
        "clocks": clocks,
        "frames": frames,
        "thresholds": {"wait_share": [WAIT_SHARE.0, WAIT_SHARE.1], "rsx_span_share": [RSX_SPAN_SHARE.0, RSX_SPAN_SHARE.1],
            "pacing_share": [PACING_SHARE.0, PACING_SHARE.1]},
    });
    if r.build_frames {
        // 63/11 only: older captures keep their exact JSON
        v["segment_replay"] = json!(r.status.as_ref().is_some_and(|s| s.flags & ST_SEGMENT_REPLAY != 0));
    }
    serde_json::to_writer(&mut *out, &v).map_err(std::io::Error::other)?;
    writeln!(out)
}

// ---- per-operation listing (`auto --ops`) -------------------------------------------------------------------------

/// One retained OPS record as `auto --ops` lists it: emission order (record id), the reached-marker interval when the
/// record is RAW_VALID, and the gap from the previous RAW_VALID record of the same frame token (its end marker to this
/// record's begin marker). Raw RSX timestamp ticks, no clock map and no verdict: the same evidence the frame verdicts
/// use, one record at a time.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OpRow {
    pub op: Op,
    pub interval: Option<u64>,
    pub gap: Option<u64>,
}

/// The records of an AUTO capture in emission order, optionally only those whose frame token is `frame`. The gap
/// is taken in EXECUTION order: among the RAW_VALID records of the same frame token sorted by begin marker, this
/// begin minus the latest end before it (a segment draw is emitted when built but runs when CALLed).
pub fn op_rows(cap: &Capture, frame: Option<u64>) -> Vec<OpRow> {
    let Some(inv) = cap.auto.as_ref() else { return Vec::new() };
    let mut ops: Vec<&Op> = inv.ops.iter().collect();
    ops.sort_by_key(|o| o.id);
    let valid = |o: &Op| o.raw_valid() && o.raw_end >= o.raw_begin;
    let mut by_token: BTreeMap<(u32, u64), Vec<usize>> = BTreeMap::new();
    for (i, o) in ops.iter().enumerate() {
        if let (true, Some(t)) = (valid(o), o.token()) { by_token.entry(t).or_default().push(i); }
    }
    let mut gaps: Vec<Option<u64>> = vec![None; ops.len()];
    for idx in by_token.values_mut() {
        idx.sort_by_key(|&i| (ops[i].raw_begin, ops[i].id));
        let mut last_end: Option<u64> = None;
        for &i in idx.iter() {
            gaps[i] = last_end.map(|e| ops[i].raw_begin.saturating_sub(e));
            last_end = Some(last_end.map_or(ops[i].raw_end, |e| e.max(ops[i].raw_end)));
        }
    }
    ops.iter().enumerate()
        .filter(|(_, o)| frame.is_none() || o.token().map(|t| t.1) == frame)
        .map(|(i, o)| OpRow { op: (*o).clone(), interval: valid(o).then(|| o.raw_end - o.raw_begin), gap: gaps[i] })
        .collect()
}

fn op_kind_name(kind: u16) -> &'static str {
    match kind {
        KIND_ARRAY => "ARRAY",
        KIND_INDEX => "INDEX",
        KIND_CLEAR => "CLEAR",
        KIND_FRAME => "FRAME",
        _ => "KIND?",
    }
}

fn op_flag_names(flags: u16) -> String {
    const NAMES: [(u16, &str); 7] = [(OP_TOKEN, "TOKEN"), (OP_RAW_VALID, "RAW_VALID"), (OP_INCOMPLETE, "INCOMPLETE"),
        (OP_CLOCK_INVALID, "CLOCK_INVALID"), (OP_READ_REFUSED, "READ_REFUSED"),
        (OP_PUBLICATION_UNKNOWN, "PUBLICATION_UNKNOWN"), (OP_SEGMENT, "SEGMENT")];
    let v: Vec<&str> = NAMES.iter().filter(|(b, _)| flags & b != 0).map(|(_, n)| *n).collect();
    if v.is_empty() { "-".into() } else { v.join("|") }
}

pub const OPS_NOTE: &str = "interval = raw_end - raw_begin and gap = this begin - the latest earlier RAW_VALID end of the \
same frame in execution (begin-marker) order, 0 when they overlap, in raw RSX report-timestamp ticks; an interval is reached-marker elapsed time between two command-stream \
positions (it includes RSX-side waits between them), not the isolated cost of one draw";

/// `auto --ops` text: one line per record, then (with a frame) the per-kind sums.
pub fn write_ops_text<W: std::io::Write>(out: &mut W, rows: &[OpRow], frame: Option<u64>) -> std::io::Result<()> {
    writeln!(out, "{OPS_NOTE}")?;
    writeln!(out, "id frame kind flags interval gap raw_begin raw_end segment call caller args")?;
    for r in rows {
        let o = &r.op;
        let fr = o.token().map_or("-".into(), |t| t.1.to_string());
        writeln!(out, "{} {} {} {} {} {} {} {} {} {} {:#x} {}", o.id, fr, op_kind_name(o.kind), op_flag_names(o.flags),
            r.interval.map_or("-".into(), |x| x.to_string()), r.gap.map_or("-".into(), |x| x.to_string()),
            o.raw_begin, o.raw_end, o.segment, o.call, o.caller,
            o.args.iter().map(|a| a.to_string()).collect::<Vec<_>>().join(","))?;
    }
    if let Some(f) = frame {
        let mut sums: BTreeMap<&str, (u64, u64, u64)> = BTreeMap::new();
        for r in rows {
            if let Some(i) = r.interval {
                let e = sums.entry(op_kind_name(r.op.kind)).or_default();
                e.0 += 1;
                e.1 = e.1.saturating_add(i);
                e.2 = e.2.saturating_add(r.gap.unwrap_or(0));
            }
        }
        for (k, (n, i, g)) in sums {
            writeln!(out, "frame {f} {k}: {n} raw-valid, interval sum {i}, gap sum {g}")?;
        }
    }
    writeln!(out, "{} record(s)", rows.len())
}

/// `auto --ops --json`: {"schema":1,"note":..,"frame":..,"ops":[..]}.
pub fn write_ops_json<W: std::io::Write>(out: &mut W, rows: &[OpRow], frame: Option<u64>) -> std::io::Result<()> {
    use serde_json::json;
    let ops: Vec<serde_json::Value> = rows.iter().map(|r| {
        let o = &r.op;
        json!({"id":o.id,"frame":o.token().map(|t| t.1),"owner":o.token().map(|t| t.0),"kind":op_kind_name(o.kind),
            "flags":o.flags,"flag_names":op_flag_names(o.flags),"interval":r.interval,"gap":r.gap,
            "raw_begin":o.raw_begin,"raw_end":o.raw_end,"tb_before":o.tb_before,"tb_after":o.tb_after,
            "caller":o.caller,"args":o.args,"segment":o.segment,"call":o.call,
            "build_frame":o.build.map(|b| b.1)})
    }).collect();
    let v = json!({"schema":1,"note":OPS_NOTE,"frame":frame,"ops":ops});
    serde_json::to_writer(&mut *out, &v).map_err(std::io::Error::other)?;
    writeln!(out)
}

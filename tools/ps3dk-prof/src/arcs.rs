//! Exact physical call observations, separate from the frozen timing stream.
use crate::{
    hash::crc32,
    model::{Clock, Malformed, ThreadInfo, MAX_CHUNK_LEN},
    wire::Cursor,
};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArcRule {
    Envelope,
    Length,
    Crc,
    Sequence,
    Order,
    Header,
    Payload,
    Lifecycle,
    Duplicate,
    Arc,
    Accounting,
    Final,
    Snapshot,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArcError {
    pub rule: ArcRule,
    pub detail: String,
}
impl std::fmt::Display for ArcError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "MALFORMED ({:?}): {}", self.rule, self.detail)
    }
}
impl std::error::Error for ArcError {}
impl From<Malformed> for ArcError {
    fn from(e: Malformed) -> Self {
        Self {
            rule: ArcRule::Payload,
            detail: e.to_string(),
        }
    }
}
fn check(ok: bool, rule: ArcRule, detail: &str) -> Result<(), ArcError> {
    if ok {
        Ok(())
    } else {
        Err(ArcError {
            rule,
            detail: detail.into(),
        })
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ArcVerdict {
    Complete,
    Incomplete(Vec<String>),
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArcHeader {
    pub app: String,
    pub build_id: Vec<u8>,
    pub sdk_version: String,
    pub compiler_identity: String,
    pub flags: String,
    pub workload_tag: String,
    pub abi: u8,
    pub mode: u8,
    pub final_snapshot: bool,
    pub session_id: u64,
    pub snapshot_id: u64,
    pub timing_prefix_bytes: u64,
    pub timing_chunk_seq: u64,
    pub timing_prefix_crc32: u32,
    pub max_threads: u32,
    pub max_arcs_per_thread: u32,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArcEntry {
    pub incarnation: u32,
    pub caller_return_pc: u64,
    pub callee_hook_pc: u64,
    pub count: u64,
    pub unknown_caller: bool,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArcFinalEntry {
    pub incarnation: u32,
    pub state: u8,
    pub limit_flags: u8,
    pub attempted: u64,
    pub recorded: u64,
    pub refused: u64,
    pub lost: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArcTrailer {
    pub end_flags: u32,
    pub sink_error_count: u64,
    pub first_sink_error: i32,
}
/// Constructed only by the checked byte reader. Incomplete captures may lack HEADER/CLOCKS.
#[derive(Debug, Clone)]
pub struct ArcCapture {
    pub(crate) header: Option<ArcHeader>,
    pub(crate) clock: Option<Clock>,
    pub(crate) threads: BTreeMap<u32, ThreadInfo>,
    pub(crate) entries: Vec<ArcEntry>,
    pub(crate) finals: BTreeMap<u32, ArcFinalEntry>,
    trailer: Option<ArcTrailer>,
    verdict: ArcVerdict,
    warnings: Vec<String>,
}
impl ArcCapture {
    pub fn header(&self) -> Option<&ArcHeader> {
        self.header.as_ref()
    }
    pub fn clock(&self) -> Option<&Clock> {
        self.clock.as_ref()
    }
    pub fn threads(&self) -> &BTreeMap<u32, ThreadInfo> {
        &self.threads
    }
    pub fn entries(&self) -> &[ArcEntry] {
        &self.entries
    }
    pub fn finals(&self) -> &BTreeMap<u32, ArcFinalEntry> {
        &self.finals
    }
    pub fn verdict(&self) -> &ArcVerdict {
        &self.verdict
    }
    pub fn warnings(&self) -> &[String] {
        &self.warnings
    }
    pub fn trailer(&self) -> Option<&ArcTrailer> {
        self.trailer.as_ref()
    }
}

fn header(c: &mut Cursor<'_>) -> Result<ArcHeader, ArcError> {
    let app = c.string()?;
    let build_id = c.raw_string()?;
    let sdk_version = c.string()?;
    let compiler_identity = c.string()?;
    let flags = c.string()?;
    let workload_tag = c.string()?;
    let abi = c.u8()?;
    let mode = c.u8()?;
    let snapshot_flags = c.u16()?;
    let h = ArcHeader {
        app,
        build_id,
        sdk_version,
        compiler_identity,
        flags,
        workload_tag,
        abi,
        mode,
        final_snapshot: snapshot_flags == 1,
        session_id: c.u64()?,
        snapshot_id: c.u64()?,
        timing_prefix_bytes: c.u64()?,
        timing_chunk_seq: c.u64()?,
        timing_prefix_crc32: c.u32()?,
        max_threads: c.u32()?,
        max_arcs_per_thread: c.u32()?,
    };
    check(
        (1..=2).contains(&abi)
            && (1..=2).contains(&mode)
            && snapshot_flags <= 1
            && h.session_id != 0
            && h.snapshot_id != 0
            && (1..=1024).contains(&h.max_threads)
            && (1..=65536).contains(&h.max_arcs_per_thread),
        ArcRule::Header,
        "invalid identity/mode/capacity",
    )?;
    check(
        if mode == 1 {
            h.timing_prefix_bytes == 0
                && h.timing_chunk_seq == u64::MAX
                && h.timing_prefix_crc32 == 0
        } else {
            h.timing_prefix_bytes >= 16 && h.timing_chunk_seq != u64::MAX
        },
        ArcRule::Header,
        "invalid timing binding",
    )?;
    Ok(h)
}

fn final_entry(
    c: &mut Cursor<'_>,
    retired: bool,
    a: &mut ArcCapture,
    sums: &BTreeMap<u32, u128>,
    live: &mut BTreeMap<u32, u32>,
    reasons: &mut Vec<String>,
) -> Result<(), ArcError> {
    let incarnation = c.u32()?;
    let state = c.u8()?;
    let limit_flags = c.u8()?;
    let reserved = c.u16()?;
    let e = ArcFinalEntry {
        incarnation,
        state,
        limit_flags,
        attempted: c.u64()?,
        recorded: c.u64()?,
        refused: c.u64()?,
        lost: c.u64()?,
    };
    check(
        a.threads.contains_key(&incarnation),
        ArcRule::Lifecycle,
        "final for undeclared incarnation",
    )?;
    check(
        !a.finals.contains_key(&incarnation),
        ArcRule::Duplicate,
        "duplicate final",
    )?;
    check(
        reserved == 0 && (1..=3).contains(&state) && (!retired || state == 1),
        ArcRule::Final,
        "invalid final state/reserved",
    )?;
    if state == 3 {
        check(
            limit_flags == 2 && [e.attempted, e.recorded, e.refused, e.lost] == [u64::MAX; 4],
            ArcRule::Final,
            "unknown counters need explicit sentinels",
        )?;
        reasons.push(format!("incarnation {incarnation}: not quiesced"));
    } else {
        check(
            limit_flags <= 1,
            ArcRule::Final,
            "invalid known counter flags",
        )?;
        check(
            u128::from(e.recorded) == sums.get(&incarnation).copied().unwrap_or(0)
                && u128::from(e.attempted)
                    == u128::from(e.recorded) + u128::from(e.refused) + u128::from(e.lost),
            ArcRule::Accounting,
            "arc/final counter equality",
        )?;
        if limit_flags != 0 || e.refused != 0 || e.lost != 0 {
            reasons.push(format!("incarnation {incarnation}: loss/refusal/limit"));
        }
    }
    if retired {
        live.remove(&a.threads[&incarnation].slot);
    }
    a.finals.insert(incarnation, e);
    Ok(())
}

pub fn read_arcs(bytes: &[u8]) -> Result<ArcCapture, ArcError> {
    check(
        bytes.len() >= 16
            && &bytes[..8] == b"PS3DKPRF"
            && bytes[8..10] == [1, 0]
            && bytes[12..16] == [1, 0, 0, 0],
        ArcRule::Envelope,
        "expected arcs v1 kind=1",
    )?;
    let mut a = ArcCapture {
        header: None,
        clock: None,
        threads: BTreeMap::new(),
        entries: vec![],
        finals: BTreeMap::new(),
        trailer: None,
        verdict: ArcVerdict::Complete,
        warnings: vec![],
    };
    if bytes[10..12] != [0, 0] {
        a.warnings
            .push("newer minor version; strict known fields only".into());
    }
    let mut pos = 16usize;
    let mut seq = 0u64;
    let mut trailer = false;
    let mut reasons = vec![];
    let mut live = BTreeMap::new();
    let mut keys = BTreeSet::new();
    let mut sums = BTreeMap::<u32, u128>::new();
    let mut sizes = BTreeMap::<u32, u32>::new();
    while pos < bytes.len() {
        check(!trailer, ArcRule::Order, "bytes after trailer")?;
        if bytes.len() - pos < 16 {
            reasons.push("torn chunk header".into());
            break;
        }
        let mut frame = Cursor::new(&bytes[pos..pos + 16], "chunk");
        let kind = frame.u32()?;
        let len = frame.u32()? as usize;
        let actual = frame.u64()?;
        check(
            len <= MAX_CHUNK_LEN as usize,
            ArcRule::Length,
            "chunk over cap",
        )?;
        check(
            actual == seq,
            ArcRule::Sequence,
            "nonconsecutive chunk sequence",
        )?;
        if bytes.len() - pos < len + 20 {
            reasons.push("torn chunk payload/checksum".into());
            break;
        }
        let end = pos + 16 + len;
        let expected = u32::from_le_bytes(bytes[end..end + 4].try_into().unwrap());
        check(
            crc32(&bytes[pos..end]) == expected,
            ArcRule::Crc,
            "chunk checksum",
        )?;
        check(
            match seq {
                0 => kind == 1,
                1 => kind == 2,
                _ => matches!(kind, 4 | 6 | 7 | 8),
            },
            ArcRule::Order,
            "chunk type/order",
        )?;
        let mut c = Cursor::new(&bytes[pos + 16..end], "arcs payload");
        match kind {
            1 => a.header = Some(header(&mut c)?),
            2 => {
                check(
                    c.u32()? == 1 && c.u32()? == 1 && c.u32()? == 0,
                    ArcRule::Payload,
                    "clock domain/count/reserved",
                )?;
                let freq_hz = c.u64()?;
                let tb_at_init = c.u64()?;
                check(freq_hz != 0, ArcRule::Payload, "zero frequency")?;
                a.clock = Some(Clock {
                    freq_hz,
                    tb_at_init,
                });
            }
            4 => {
                let n = c.u32()?;
                check(n > 0, ArcRule::Lifecycle, "empty thread declaration")?;
                for _ in 0..n {
                    let t = ThreadInfo {
                        incarnation: c.u32()?,
                        slot: c.u32()?,
                        os_thread_id: c.u64()?,
                        name: c.string()?,
                    };
                    check(
                        t.incarnation != 0
                            && t.slot < a.header.as_ref().unwrap().max_threads
                            && !a.threads.contains_key(&t.incarnation)
                            && !live.contains_key(&t.slot),
                        ArcRule::Lifecycle,
                        "incarnation/slot not available",
                    )?;
                    live.insert(t.slot, t.incarnation);
                    a.threads.insert(t.incarnation, t);
                }
            }
            8 => {
                let inc = c.u32()?;
                let n = c.u32()?;
                check(
                    n > 0 && len as u64 == 8 + 32 * u64::from(n),
                    ArcRule::Length,
                    "arc payload length",
                )?;
                check(
                    a.threads.contains_key(&inc) && !a.finals.contains_key(&inc),
                    ArcRule::Lifecycle,
                    "arcs need live declaration",
                )?;
                for _ in 0..n {
                    let caller = c.u64()?;
                    let callee = c.u64()?;
                    let count = c.u64()?;
                    let flags = c.u32()?;
                    let reserved = c.u32()?;
                    check(
                        reserved == 0
                            && flags <= 1
                            && count != 0
                            && callee != 0
                            && callee % 4 == 0
                            && if flags == 1 {
                                caller == 0
                            } else {
                                caller != 0 && caller % 4 == 0
                            },
                        ArcRule::Arc,
                        "count/PC/flags invalid",
                    )?;
                    check(
                        keys.insert((inc, caller, callee, flags)),
                        ArcRule::Duplicate,
                        "duplicate arc key",
                    )?;
                    let size = sizes.entry(inc).or_default();
                    *size += 1;
                    check(
                        *size <= a.header.as_ref().unwrap().max_arcs_per_thread,
                        ArcRule::Arc,
                        "arc capacity exceeded",
                    )?;
                    *sums.entry(inc).or_default() += u128::from(count);
                    a.entries.push(ArcEntry {
                        incarnation: inc,
                        caller_return_pc: caller,
                        callee_hook_pc: callee,
                        count,
                        unknown_caller: flags == 1,
                    });
                }
            }
            7 => {
                let n = c.u32()?;
                check(n > 0, ArcRule::Final, "empty retirement")?;
                for _ in 0..n {
                    final_entry(&mut c, true, &mut a, &sums, &mut live, &mut reasons)?;
                }
            }
            6 => {
                let flags = c.u32()?;
                let sink = c.u64()?;
                let error = c.i32()?;
                let n = c.u32()?;
                for _ in 0..n {
                    final_entry(&mut c, false, &mut a, &sums, &mut live, &mut reasons)?;
                }
                let required = u32::from(a.finals.values().any(|e| e.state == 3))
                    | (u32::from(a.finals.values().any(|e| e.limit_flags & 1 != 0)) << 1)
                    | (u32::from(sink != 0) << 2);
                check(
                    flags == required && (sink == 0) == (error == 0),
                    ArcRule::Final,
                    "end flags/sink status disagree",
                )?;
                a.trailer = Some(ArcTrailer {
                    end_flags: flags,
                    sink_error_count: sink,
                    first_sink_error: error,
                });
                if sink != 0 {
                    reasons.push("sink error".into());
                }
                trailer = true;
            }
            _ => unreachable!(),
        }
        c.done()?;
        pos = end + 4;
        seq = seq.checked_add(1).ok_or_else(|| ArcError {
            rule: ArcRule::Sequence,
            detail: "sequence overflow".into(),
        })?;
    }
    if !trailer {
        reasons.push("missing trailer".into());
    }
    for inc in a.threads.keys() {
        if !a.finals.contains_key(inc) {
            reasons.push(format!("incarnation {inc}: missing final"));
        }
    }
    if !reasons.is_empty() {
        a.verdict = ArcVerdict::Incomplete(reasons);
    }
    Ok(a)
}

#[derive(Default)]
pub struct SnapshotSeries {
    current: Option<ArcCapture>,
}
impl SnapshotSeries {
    pub fn current(&self) -> Option<&ArcCapture> {
        self.current.as_ref()
    }
    pub fn accept(&mut self, next: ArcCapture) -> Result<(), ArcError> {
        let n = next.header.as_ref().ok_or_else(|| ArcError {
            rule: ArcRule::Snapshot,
            detail: "snapshot has no identity".into(),
        })?;
        if let Some(old) = &self.current {
            let p = old.header.as_ref().unwrap();
            check(
                n.session_id == p.session_id
                    && n.build_id == p.build_id
                    && n.abi == p.abi
                    && n.mode == p.mode
                    && n.app == p.app
                    && n.sdk_version == p.sdk_version
                    && n.compiler_identity == p.compiler_identity
                    && n.flags == p.flags
                    && n.workload_tag == p.workload_tag
                    && next.clock == old.clock
                    && n.snapshot_id > p.snapshot_id,
                ArcRule::Snapshot,
                "snapshot identity changed or ID did not advance",
            )?;
        }
        self.current = Some(next);
        Ok(())
    }
}

/// Host-only JSON: exact u64 values are strings, never floating-point JSON numbers.
pub fn export_arcs_json(a: &ArcCapture) -> String {
    use serde_json::json;
    let identity=a.header.as_ref().map(|h|json!({"app":h.app,"build_id":h.build_id.iter().map(|v|format!("{v:02x}")).collect::<String>(),
        "sdk_version":h.sdk_version,"compiler_identity":h.compiler_identity,"flags":h.flags,"workload_tag":h.workload_tag,
        "abi":h.abi,"mode":h.mode,"session_id":h.session_id.to_string(),"snapshot_id":h.snapshot_id.to_string(),
        "final_snapshot":h.final_snapshot,"timing_prefix_bytes":h.timing_prefix_bytes.to_string(),
        "timing_chunk_seq":h.timing_chunk_seq.to_string(),"timing_prefix_crc32":h.timing_prefix_crc32,
        "max_threads":h.max_threads,"max_arcs_per_thread":h.max_arcs_per_thread}));
    let rows=a.entries.iter().map(|e|json!({"incarnation":e.incarnation,"caller":format!("0x{:016x}",e.caller_return_pc),
        "callee":format!("0x{:016x}",e.callee_hook_pc),"count":e.count.to_string(),"unknown_caller":e.unknown_caller})).collect::<Vec<_>>();
    let finals=a.finals.values().map(|e|json!({"incarnation":e.incarnation,"state":e.state,"limit_flags":e.limit_flags,
        "attempted":e.attempted.to_string(),"recorded":e.recorded.to_string(),"refused":e.refused.to_string(),"lost":e.lost.to_string()})).collect::<Vec<_>>();
    let threads=a.threads.values().map(|t|json!({"incarnation":t.incarnation,"slot":t.slot,"os_thread_id":t.os_thread_id.to_string(),"name":t.name})).collect::<Vec<_>>();
    let reasons = match &a.verdict {
        ArcVerdict::Complete => &[][..],
        ArcVerdict::Incomplete(r) => r.as_slice(),
    };
    json!({"identity":identity,"clock":a.clock.as_ref().map(|c|json!({"freq_hz":c.freq_hz.to_string(),"tb_at_init":c.tb_at_init.to_string()})),
        "complete":a.verdict==ArcVerdict::Complete,"reasons":reasons,"warnings":a.warnings,"threads":threads,"arcs":rows,"finals":finals,
        "trailer":a.trailer.as_ref().map(|t|json!({"end_flags":t.end_flags,"sink_error_count":t.sink_error_count.to_string(),"first_sink_error":t.first_sink_error}))}).to_string()
}

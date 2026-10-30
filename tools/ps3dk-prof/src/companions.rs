//! Bind a cumulative arc snapshot to exactly the timing bytes it names.
use crate::{
    arcs::{ArcCapture, ArcVerdict},
    hash::crc32,
    model::Verdict,
    Analysis, Outcome,
};

#[derive(Debug, Clone)]
pub struct VerifiedPair {
    pub(crate) arcs: ArcCapture,
    pub(crate) timing: Analysis,
    final_snapshot: bool,
}
impl VerifiedPair {
    pub fn is_final(&self) -> bool {
        self.final_snapshot
    }
    pub fn arcs(&self) -> &ArcCapture {
        &self.arcs
    }
    pub fn timing(&self) -> &Analysis {
        &self.timing
    }
}
pub fn match_companions(arcs: &ArcCapture, bytes: &[u8]) -> Result<VerifiedPair, String> {
    let h = arcs.header().ok_or("missing arc header")?;
    if h.mode != 2 {
        return Err("pg-only snapshot has no timing binding".into());
    }
    let len = usize::try_from(h.timing_prefix_bytes).map_err(|_| "prefix cannot be addressed")?;
    let prefix = bytes.get(..len).ok_or("missing timing prefix")?;
    if h.final_snapshot && len != bytes.len() {
        return Err("final snapshot requires whole timing file".into());
    }
    if crc32(prefix) != h.timing_prefix_crc32 {
        return Err("timing prefix checksum mismatch".into());
    }
    // Check the exact boundary before the timing parser's intentionally permissive torn-tail handling.
    let mut pos = 16usize;
    let mut last = None;
    while pos < len {
        let head = prefix
            .get(pos..pos + 16)
            .ok_or("bound prefix ends inside chunk header")?;
        let n = u32::from_le_bytes(head[4..8].try_into().unwrap()) as usize;
        pos = pos.checked_add(n + 20).ok_or("prefix length overflow")?;
        if pos > len {
            return Err("bound prefix ends inside chunk".into());
        }
        last = Some(u64::from_le_bytes(head[8..16].try_into().unwrap()));
    }
    if pos != len || last != Some(h.timing_chunk_seq) {
        return Err("timing frontier sequence mismatch".into());
    }
    let analysis = match crate::load(prefix) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(e) => return Err(e.to_string()),
    };
    let t = analysis
        .capture
        .header
        .as_ref()
        .ok_or("missing timing header")?;
    if h.app != t.app
        || h.build_id != t.build_id
        || h.sdk_version != t.sdk_version
        || h.compiler_identity != t.compiler_identity
        || h.flags != t.flags
        || h.workload_tag != t.workload_tag
        || h.abi != t.abi
        || h.session_id != t.session_id
        || arcs.clock() != analysis.capture.clock.as_ref()
    {
        return Err("companion session/build/ABI/clock identity mismatch".into());
    }
    if arcs.threads().len() != analysis.capture.lanes.len()
        || arcs.threads().iter().any(|(inc, t)| {
            analysis
                .capture
                .lanes
                .get(inc)
                .and_then(|l| l.info.as_ref())
                != Some(t)
        })
    {
        return Err("companion thread identity set mismatch".into());
    }
    if h.final_snapshot
        && (arcs.verdict() != &ArcVerdict::Complete || analysis.verdict != Verdict::Complete)
    {
        return Err("final companions must both be complete".into());
    }
    Ok(VerifiedPair {
        arcs: arcs.clone(),
        timing: analysis,
        final_snapshot: h.final_snapshot,
    })
}

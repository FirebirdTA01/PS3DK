//! Admission inventory for later source proof. An admission is not completed work.
use crate::checkpoints::{Frame, Source};
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const CHUNK_ADMISSION: u32 = 25;
pub const CHUNK_SOURCE_SEAL: u32 = 26;

#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub entries: u64,
    pub bytes: u64,
    pub ops: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self {
            entries: 262144,
            bytes: 64 * 1024 * 1024,
            ops: 100_000_000,
        }
    }
}

#[cfg(test)]
#[path = "../tests/common/mod.rs"]
mod fixture;

#[cfg(test)]
mod tests {
    use super::fixture::*;
    use super::*;

    fn file(items: &[(u64, u64)]) -> File {
        let mut b = Buf::default();
        b.u16(1)
            .u16(0)
            .u32(items.len() as u32)
            .u8(1)
            .u8(0)
            .u16(0)
            .u32(1)
            .u32(0)
            .u32(0);
        for &(id, begin) in items {
            b.u32(1).u32(0).u64(0).u64(id).u64(begin);
        }
        let mut f = File::new(vec![
            chunk(T_HEADER, Hdr::default().payload()),
            chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
            chunk(T_THREADS, threads_payload(&[thr(1)])),
            chunk(T_RECORDS, records_payload(1, &[frame(0, 10, 0)])),
            chunk(25, b.0),
        ]);
        f.flags = 8;
        f
    }

    #[test]
    fn parser_commit_trace_keeps_prior_batch_but_not_invalid_final_item() {
        let mut f = file(&[(1, 2)]);
        f.chunks.push(file(&[(2, 4), (2, 6)]).chunks.pop().unwrap());
        COMMITTED.with(|t| t.borrow_mut().clear());
        let m = crate::reader::read(&f.bytes()).unwrap_err();
        assert_eq!(m.rule, Rule::Checkpoint);
        assert!(m.detail.contains("ordinal"));
        COMMITTED.with(|t| assert_eq!(*t.borrow(), [(4, 1)]));
    }

    #[test]
    fn gaps_latch_uncertainty_without_inventing_an_admission() {
        let mut f = file(&[]);
        f.chunks.pop();
        let cap = crate::reader::read(&f.bytes()).unwrap();
        let membership = crate::checkpoints::Reader::new(Default::default());
        let mut r = Reader::new(Default::default());
        for (seq, items) in [(4, vec![(2, 4)]), (5, vec![(3, 6)])] {
            let f = file(&items);
            r.chunk(
                25,
                &f.chunks.last().unwrap().payload,
                seq,
                &cap,
                None,
                &membership,
            )
            .unwrap();
        }
        assert_eq!(r.admissions.len(), 2);
        assert_eq!(
            r.admissions.iter().map(|a| a.id).collect::<Vec<_>>(),
            [2, 3]
        );
        assert!(r.sources[0].uncertain);
        assert!(r.result.results.is_empty());
    }
    #[test]
    fn source_contradiction_prevents_dependency_publication() {
        let mut f = file(&[(1, 2), (2, 5)]);
        f.chunks[3] = chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]),
        );
        f.chunks
            .push(chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])));
        f.chunks.push(chunk(
            T_RECORDS,
            records_payload(
                1,
                &[
                    begin(2, 1, 0, 30, 1),
                    bparent_tok(3, 0, 0, 1, 0),
                    end(4, 1, 0, 40, 1),
                    begin(5, 1, 0, 50, 2),
                    bparent_tok(6, 0, 0, 1, 1),
                    end(7, 1, 0, 60, 2),
                ],
            ),
        ));
        crate::dependencies::COMMITTED.with(|t| t.borrow_mut().clear());
        let m = crate::reader::read(&f.bytes()).unwrap_err();
        assert!(m.detail.contains("token"), "{m:?}");
        crate::dependencies::COMMITTED.with(|t| assert_eq!(*t.borrow(), [(3, 2)]));
    }
    #[test]
    fn dependency_commit_refusal_cannot_be_silent_no_data() {
        let mut f = file(&[]);
        f.chunks.pop();
        f.chunks[3] = chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]),
        );
        let mut close = Buf::default();
        close
            .u16(1)
            .u16(0)
            .u32(1)
            .u64(0)
            .u64(1)
            .u64(0)
            .u64(0)
            .u32(1)
            .u32(0);
        close.u8(1).u8(0).u16(0).u32(1).u32(0).u32(0);
        f.chunks.push(chunk(20, close.0));
        let mut seal = Buf::default();
        seal.u16(1)
            .u16(0)
            .u32(1)
            .u64(0)
            .u8(1)
            .u8(0)
            .u16(0)
            .u32(1)
            .u32(0)
            .u32(0);
        seal.u64(4).u64(0).u64(0).u64(0).u64(0).u64(0);
        f.chunks.push(chunk(26, seal.0));
        let mut terminal = Buf::default();
        terminal
            .u16(1)
            .u8(2)
            .u8(0)
            .u32(1)
            .u64(0)
            .u8(1)
            .u8(0)
            .u16(0)
            .u32(1)
            .u32(0)
            .u32(0);
        terminal.u64(4).u64(5).u64(0).u32(0).u32(0);
        f.chunks.push(chunk(21, terminal.0));
        crate::dependencies::FAIL_NEXT_COMMIT.with(|v| v.set(true));
        let c = crate::reader::read(&f.bytes()).unwrap();
        assert!(c.dependency_status().unwrap().refusal.is_some());
        let proof = c.source_checkpoints.unwrap();
        assert!(proof.results.is_empty());
        assert_eq!(proof.refusal.unwrap().limit, Limit::Dependencies);
    }
    #[test]
    fn admission_peak_storage_exact_one_short_and_allocation_refusal() {
        let mut base = file(&[]);
        base.chunks.pop();
        let cap = crate::reader::read(&base.bytes()).unwrap();
        let membership = crate::checkpoints::Reader::new(Default::default());
        let payload = file(&[(1, 2)]).chunks.pop().unwrap().payload;
        let a = std::mem::size_of::<Admission>() as u64;
        let s = std::mem::size_of::<SourceState>() as u64;
        let peak = 2 * a + s;
        for (bytes, refused) in [(peak - 1, true), (peak, false)] {
            let mut r = Reader::new(Budgets {
                bytes,
                ..Default::default()
            });
            r.chunk(25, &payload, 4, &cap, None, &membership).unwrap();
            assert_eq!(r.result.refusal.is_some(), refused);
            assert_eq!(r.admissions.len(), usize::from(!refused));
            if !refused {
                assert_eq!(r.result.accounted_bytes, a + s);
            }
        }
        let mut r = Reader::new(Default::default());
        FAIL_ALLOCATION.with(|v| v.set(true));
        r.chunk(25, &payload, 4, &cap, None, &membership).unwrap();
        assert_eq!(r.result.refusal.unwrap().limit, Limit::Allocation);
        assert!(r.admissions.is_empty());
        println!("host sizes Admission={a} SourceState={s} Begin={} Seal={} SourceResult={} Pending={}; first admission peak={peak}",
            std::mem::size_of::<Begin>(),std::mem::size_of::<Seal>(),std::mem::size_of::<SourceResult>(),std::mem::size_of::<Pending>());
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Limit {
    Entries,
    Storage,
    Work,
    Allocation,
    Membership,
    Dependencies,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Refusal {
    pub limit: Limit,
    pub chunk: u64,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    CompleteRecorded,
    NoData,
    Partial,
    Unknown,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceResult {
    pub frame: Frame,
    pub source: Source,
    pub outcome: Outcome,
    pub reasons: u64,
    /// Independently observed predicates; separate from producer reason bits.
    pub observed_reasons: u64,
    pub close_chunk: u64,
    pub seal_chunk: u64,
    pub terminal_chunk: u64,
    pub resolved: u64,
}
#[derive(Debug, Clone)]
pub struct Pending {
    pub frame: Frame,
    pub source: Source,
    pub missing: &'static str,
}
#[derive(Debug, Clone, Default)]
pub struct Evidence {
    pub results: Vec<SourceResult>,
    pub pending: Vec<Pending>,
    pub refusal: Option<Refusal>,
    pub used_ops: u64,
    pub accounted_bytes: u64,
    pub stream_seen: bool,
    pub admissions: u64,
    pub limits: Budgets,
}
pub fn report(e: &Evidence) -> String {
    use std::fmt::Write;
    if !e.stream_seen {
        return String::new();
    }
    let mut out = String::new();
    let state = match e.refusal {
        Some(r) => format!(
            "INCOMPLETE ({} at chunk {})",
            match r.limit {
                Limit::Entries => "entry limit",
                Limit::Storage => "storage limit",
                Limit::Work => "work limit",
                Limit::Allocation => "allocation refusal",
                Limit::Membership => "membership evidence unavailable",
                Limit::Dependencies => "dependency evidence unavailable",
            },
            r.chunk
        ),
        None => "VALIDATED RECORDED CLAIMS".to_owned(),
    };
    let _ = writeln!(out,"Source evidence: {state}; retained admissions {}, terminals {}; accounted bytes {}, used work {}; PC policy ceilings entries {}, bytes {}, work {}. Completion does not establish timing validity or storage durability.",
        e.admissions,e.results.len(),e.accounted_bytes,e.used_ops,e.limits.entries,e.limits.bytes,e.limits.ops);
    for result in &e.results {
        let name = match result.outcome {
            Outcome::CompleteRecorded => "COMPLETE_RECORDED",
            Outcome::NoData => "NO_DATA",
            Outcome::Partial => "PARTIAL",
            Outcome::Unknown => "UNKNOWN",
        };
        let _ = write!(
            out,
            "  owner {} frame {} source {}/{}/{}: {name}; resolved {}",
            result.frame.owner,
            result.frame.id,
            result.source.kind,
            result.source.id,
            result.source.domain,
            result.resolved
        );
        for (bit, text) in [
            "pre-close refusal",
            "dependency loss",
            "unresolved admission",
            "membership unknown",
            "admission inventory unknown",
            "attribution unknown",
            "epoch mismatch",
            "unsupported SPU job/frame",
            "producer-reported failure",
            "producer-reported capacity",
        ]
        .iter()
        .enumerate()
        {
            if (result.reasons | result.observed_reasons) & (1 << bit) != 0 {
                let _ = write!(out, "; {text}");
            }
        }
        out.push('\n');
    }
    for p in &e.pending {
        let _ = writeln!(
            out,
            "  owner {} frame {} source {}/{}/{}: PENDING ({})",
            p.frame.owner, p.frame.id, p.source.kind, p.source.id, p.source.domain, p.missing
        );
    }
    if e.refusal.is_some() {
        out.push_str("  pending inventory UNAVAILABLE; prior validated terminals retained\n");
    }
    out
}
#[derive(Debug, Clone, Copy)]
struct Admission {
    source: Source,
    frame: Frame,
    epoch: u32,
    id: u64,
    begin: u64,
    chunk: u64,
}
#[derive(Debug, Clone, Copy)]
struct SourceState {
    source: Source,
    last: u64,
    seen: u64,
    lost: u64,
    uncertain: bool,
    participating: bool,
    attribution: bool,
}
impl SourceState {
    fn new(source: Source) -> Self {
        Self {
            source,
            last: 0,
            seen: 0,
            lost: 0,
            uncertain: false,
            participating: false,
            attribution: false,
        }
    }
}
#[derive(Debug, Clone, Copy)]
struct Begin {
    source: Source,
    frame: Option<Frame>,
    seq: u64,
    epoch: u32,
    chunk: u64,
}
#[derive(Debug, Clone, Copy)]
struct Seal {
    source: Source,
    frame: Frame,
    chunk: u64,
    close_chunk: u64,
    admitted: u64,
    refused: u64,
    admission_unknown: bool,
    membership_unknown: bool,
}
#[derive(Debug, Clone, Copy)]
enum Resolution {
    Valid(u64),
    Lost,
    Unresolved,
    Epoch,
    Attribution,
    Policy,
    InvalidIdentity,
}
fn resolve(a: &Admission, cap: &Capture, before: u64) -> Resolution {
    use crate::dependencies::{Expected, Lookup, QueryBudget, RecordId, UnavailableReason};
    fn bad<T>(r: Lookup<T>) -> Resolution {
        match r {
            Lookup::Lost { .. } => Resolution::Lost,
            Lookup::Unavailable(UnavailableReason::WrongEpoch) => Resolution::Epoch,
            Lookup::Unavailable(UnavailableReason::WrongKind) => Resolution::InvalidIdentity,
            Lookup::Unavailable(
                UnavailableReason::QueryBudget
                | UnavailableReason::IndexRefused
                | UnavailableReason::HistoryNotIndexed,
            ) => Resolution::Policy,
            _ => Resolution::Unresolved,
        }
    }
    let id = RecordId {
        source: a.source,
        seq: a.begin,
    };
    let expected = Expected {
        epoch: (a.source.kind != 1).then_some(a.epoch),
        kind: Some(1),
    };
    let begin = match cap.dependency_record(id, expected, before, QueryBudget::default()) {
        Lookup::Present(v) => v,
        x => return bad(x),
    };
    let pair = match cap.dependency_pair(id, expected, before, QueryBudget::default()) {
        Lookup::Present(v) => v,
        x => return bad(x),
    };
    if pair.truncated_at_stop || pair.affected_at_match {
        return Resolution::Unresolved;
    }
    let token = if a.source.kind == 1 {
        let Some(parent) = pair.parent else {
            return Resolution::Unresolved;
        };
        match cap.dependency_record(
            parent,
            Expected {
                epoch: None,
                kind: Some(2),
            },
            before,
            QueryBudget::default(),
        ) {
            Lookup::Present(v) => v.token,
            x => return bad(x),
        }
    } else {
        begin.token
    };
    if token != Some(a.frame) {
        return Resolution::Attribution;
    }
    let end = match cap.dependency_record(
        pair.end,
        Expected {
            epoch: expected.epoch,
            kind: Some(if a.source.kind == 1 { 3 } else { 2 }),
        },
        before,
        QueryBudget::default(),
    ) {
        Lookup::Present(v) => v,
        x => return bad(x),
    };
    if a.source.kind != 1 && end.token != Some(a.frame) {
        return Resolution::Attribution;
    }
    Resolution::Valid(pair.end.seq)
}

pub(crate) struct Reader {
    pub result: Evidence,
    admissions: Vec<Admission>,
    sources: Vec<SourceState>,
    seals: Vec<Seal>,
    begins: Vec<Begin>,
}
struct Prepared {
    items: Vec<Admission>,
    state: SourceState,
    slot: Result<usize, usize>,
    admissions: Option<Vec<Admission>>,
    sources: Option<Vec<SourceState>>,
    chunk: u64,
}
#[cfg(test)]
std::thread_local! { static COMMITTED: std::cell::RefCell<Vec<(u64, usize)>> = const { std::cell::RefCell::new(Vec::new()) }; }
#[cfg(test)]
std::thread_local! { static FAIL_ALLOCATION: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }
fn fail<T>(s: &str) -> Result<T, Malformed> {
    malformed(Rule::Checkpoint, s)
}
fn exact<T>(n: usize) -> Option<Vec<T>> {
    #[cfg(test)]
    if FAIL_ALLOCATION.with(|v| v.replace(false)) {
        return None;
    }
    let mut v = Vec::new();
    v.try_reserve_exact(n).ok()?;
    (v.capacity() == n).then_some(v)
}
fn bytes<T>(n: usize) -> Option<u64> {
    (n as u64).checked_mul(std::mem::size_of::<T>() as u64)
}

impl Reader {
    pub fn new(limits: Budgets) -> Self {
        Self {
            result: Evidence {
                limits,
                ..Evidence::default()
            },
            admissions: Vec::new(),
            sources: Vec::new(),
            seals: Vec::new(),
            begins: Vec::new(),
        }
    }
    fn refuse(&mut self, limit: Limit, chunk: u64) {
        self.result.refusal.get_or_insert(Refusal { limit, chunk });
    }
    pub fn chunk(
        &mut self,
        ty: u32,
        payload: &[u8],
        chunk: u64,
        cap: &Capture,
        ext: Option<&crate::ext::ExtCapture>,
        membership: &crate::checkpoints::Reader,
    ) -> Result<(), Malformed> {
        if ty == CHUNK_SOURCE_SEAL {
            return self.seal(payload, chunk, membership);
        }
        if ty == 21 {
            return self.terminal(payload, chunk, cap);
        }
        if payload.len() < 24 {
            return fail("admission payload length");
        }
        let n = u32::from_le_bytes(payload[4..8].try_into().unwrap()) as usize;
        if n == 0 {
            return fail("admission count is zero");
        }
        if n.checked_mul(32).and_then(|v| v.checked_add(24)) != Some(payload.len()) {
            return fail("admission payload length");
        }
        self.result.stream_seen = true;
        if self.result.refusal.is_some() {
            return Ok(());
        }
        if let Some(prepared) = self.prepare(payload, n, chunk, cap, ext, membership)? {
            self.commit(prepared);
        }
        Ok(())
    }
    fn prepare(
        &mut self,
        payload: &[u8],
        n: usize,
        chunk: u64,
        cap: &Capture,
        ext: Option<&crate::ext::ExtCapture>,
        membership: &crate::checkpoints::Reader,
    ) -> Result<Option<Prepared>, Malformed> {
        let Some(total) = self.admissions.len().checked_add(n) else {
            self.refuse(Limit::Entries, chunk);
            return Ok(None);
        };
        if (total as u64)
            .checked_add(self.seals.len() as u64)
            .and_then(|v| v.checked_add(self.begins.len() as u64))
            .and_then(|v| v.checked_add(self.result.results.len() as u64))
            .is_none_or(|v| v > self.result.limits.entries)
        {
            self.refuse(Limit::Entries, chunk);
            return Ok(None);
        }
        // Charge decoding, declared-source/owner lookups, membership scans and
        // duplicate-BEGIN checks before traversing any of those populations.
        let work = (|| {
            let scan = membership
                .admission_scan_len()
                .checked_add(cap.lanes.len() as u64)?
                // Frame lookup is a binary search, at most 64 comparisons.
                .checked_add(64)?
                .checked_add(self.sources.len() as u64)?
                .checked_add(total as u64)?
                .checked_add(self.begins.len() as u64)?;
            (n as u64)
                .checked_mul(scan.checked_add(1)?)?
                .checked_mul(64)?
                .checked_add(payload.len() as u64)?
                .checked_add((n as u64).checked_mul(4096)?)?
                .checked_add(total as u64)
        })();
        let Some(ops) = work
            .and_then(|v| self.result.used_ops.checked_add(v))
            .filter(|&v| v <= self.result.limits.ops)
        else {
            self.refuse(Limit::Work, chunk);
            return Ok(None);
        };
        self.result.used_ops = ops;
        let mut c = Cursor::new(payload, "admission");
        if c.u16()? != 1 {
            return fail("admission schema");
        }
        if c.u16()? != 0 {
            return fail("admission reserved field");
        }
        c.u32()?;
        let source = crate::checkpoints::key(&mut c)?;
        if !crate::checkpoints::declared(source, cap, ext) {
            return fail("admission source is not declared");
        }
        let slot = self.sources.binary_search_by_key(&source, |s| s.source);
        let mut state = slot
            .ok()
            .map(|i| self.sources[i])
            .unwrap_or(SourceState::new(source));
        let source_total = self.sources.len() + usize::from(slot.is_err());
        let grow_items = total > self.admissions.capacity();
        let grow_sources = source_total > self.sources.capacity();
        let peak = (|| {
            self.result
                .accounted_bytes
                .checked_add(bytes::<Admission>(n)?)?
                .checked_add(if grow_items {
                    bytes::<Admission>(total)?
                } else {
                    0
                })?
                .checked_add(if grow_sources {
                    bytes::<SourceState>(source_total)?
                } else {
                    0
                })
        })();
        if peak.is_none_or(|v| v > self.result.limits.bytes) {
            self.refuse(Limit::Storage, chunk);
            return Ok(None);
        }
        let Some(mut items) = exact::<Admission>(n) else {
            self.refuse(Limit::Allocation, chunk);
            return Ok(None);
        };
        for _ in 0..n {
            let owner = c.u32()?;
            let epoch = c.u32()?;
            let frame = Frame {
                owner,
                id: c.u64()?,
            };
            let id = c.u64()?;
            let begin = c.u64()?;
            if id <= state.last || id == u64::MAX {
                return fail("admission ordinal regressed or exhausted");
            }
            if begin == u64::MAX {
                return fail("admission BEGIN sequence exhausted");
            }
            let Some(lane) = cap.lanes.get(&owner).filter(|lane| lane.info.is_some()) else {
                return fail("admission frame owner is not declared");
            };
            if lane
                .frames
                .binary_search_by_key(&frame.id, |f| f.id)
                .is_err()
            {
                return fail("admission frame is not recorded");
            }
            if source.kind == 1 {
                if epoch != 0 {
                    return fail("PPU admission epoch must be zero");
                }
            } else if epoch == 0
                || ext
                    .and_then(|x| x.domains.get(&source.domain))
                    .is_none_or(|d| epoch > d.epochs)
            {
                return fail("admission epoch is not declared");
            }
            membership.admission_open(frame)?;
            match membership.admission_member(frame, source) {
                Some(false) => return fail("admission has no preceding membership"),
                // Membership uncertainty is carried by the CLOSE/SEAL. It is
                // not evidence of a missing admission ordinal.
                None => {}
                Some(true) => {}
            }
            use crate::dependencies::{Expected, Lookup, QueryBudget, RecordId};
            match cap.dependency_record(
                RecordId { source, seq: begin },
                Expected::default(),
                chunk,
                QueryBudget::default(),
            ) {
                Lookup::Present(r) if r.kind != 1 => {
                    return fail("admission identity names a known non-BEGIN record")
                }
                Lookup::Unavailable(_) => {
                    self.refuse(Limit::Dependencies, chunk);
                    return Ok(None);
                }
                _ => {}
            }
            if self
                .admissions
                .iter()
                .chain(items.iter())
                .any(|a| a.source == source && a.begin == begin)
            {
                return fail("admission reuses BEGIN identity");
            }
            if self.begins.iter().any(|b| {
                b.source == source
                    && b.seq == begin
                    && ((source.kind != 2 && b.frame.is_some_and(|f| f != frame))
                        || b.epoch != epoch)
            }) {
                return fail("admission token or epoch contradicts recorded BEGIN");
            }
            state.uncertain |= id != state.last + 1;
            state.last = id;
            state.seen += 1; // bounded by the entry limit
            items.push(Admission {
                source,
                frame,
                epoch,
                id,
                begin,
                chunk,
            });
        }
        c.done()?;
        // All potentially failing allocation happens before publication.
        let admissions = if grow_items {
            let Some(v) = exact(total) else {
                self.refuse(Limit::Allocation, chunk);
                return Ok(None);
            };
            Some(v)
        } else {
            None
        };
        let sources = if grow_sources {
            let Some(v) = exact(source_total) else {
                self.refuse(Limit::Allocation, chunk);
                return Ok(None);
            };
            Some(v)
        } else {
            None
        };
        Ok(Some(Prepared {
            items,
            state,
            slot,
            admissions,
            sources,
            chunk,
        }))
    }
    fn commit(&mut self, mut p: Prepared) {
        if let Some(mut v) = p.admissions.take() {
            v.append(&mut self.admissions);
            self.admissions = v;
        }
        if let Some(mut v) = p.sources.take() {
            v.append(&mut self.sources);
            self.sources = v;
        }
        let n = p.items.len();
        self.admissions.append(&mut p.items);
        match p.slot {
            Ok(i) => self.sources[i] = p.state,
            Err(i) => self.sources.insert(i, p.state),
        }
        self.result.admissions = self.admissions.len() as u64;
        self.account();
        #[cfg(test)]
        COMMITTED.with(|v| v.borrow_mut().push((p.chunk, n)));
        #[cfg(not(test))]
        let _ = (p.chunk, n);
    }

    fn account(&mut self) {
        self.result.accounted_bytes = bytes::<Admission>(self.admissions.capacity()).unwrap()
            + bytes::<SourceState>(self.sources.capacity()).unwrap()
            + bytes::<Seal>(self.seals.capacity()).unwrap()
            + bytes::<Begin>(self.begins.capacity()).unwrap()
            + bytes::<SourceResult>(self.result.results.capacity()).unwrap();
    }
    fn seal(
        &mut self,
        payload: &[u8],
        chunk: u64,
        membership: &crate::checkpoints::Reader,
    ) -> Result<(), Malformed> {
        if payload.len() != 80 {
            return fail("source seal payload length");
        }
        self.result.stream_seen = true;
        if self.result.refusal.is_some() {
            return Ok(());
        }
        let total = self.seals.len() + 1;
        if (self.admissions.len() as u64)
            .checked_add(total as u64)
            .and_then(|v| v.checked_add(self.begins.len() as u64))
            .and_then(|v| v.checked_add(self.result.results.len() as u64))
            .is_none_or(|n| n > self.result.limits.entries)
        {
            self.refuse(Limit::Entries, chunk);
            return Ok(());
        }
        let work = (|| {
            (self.admissions.len() as u64)
                .checked_add(self.sources.len() as u64)?
                .checked_add(total as u64)?
                .checked_add(membership.result.closed.len() as u64)?
                .checked_add(1)?
                .checked_mul(128)?
                .checked_add(80)?
                .checked_add(self.result.used_ops)
        })();
        let Some(work) = work.filter(|&n| n <= self.result.limits.ops) else {
            self.refuse(Limit::Work, chunk);
            return Ok(());
        };
        self.result.used_ops = work;
        if membership.result.refusal.is_some() {
            self.refuse(Limit::Membership, chunk);
            return Ok(());
        }
        let mut c = Cursor::new(payload, "source seal");
        if c.u16()? != 1 {
            return fail("source seal schema");
        }
        if c.u16()? != 0 {
            return fail("source seal reserved field");
        }
        let frame = Frame {
            owner: c.u32()?,
            id: c.u64()?,
        };
        let source = crate::checkpoints::key(&mut c)?;
        let close_chunk = c.u64()?;
        let attempts = c.u64()?;
        let lost = c.u64()?;
        let admitted = c.u64()?;
        let refused = c.u64()?;
        if c.u64()? != 0 {
            return fail("source seal reserved field");
        }
        c.done()?;
        let Some(close) = membership.result.closed.get(&frame) else {
            return fail("source seal lacks preceding CLOSE");
        };
        if close.chunk_seq != close_chunk || close_chunk >= chunk {
            return fail("source seal CLOSE reference differs");
        }
        if close.members.binary_search(&source).is_err() {
            return fail("source seal source not in CLOSE roster");
        }
        let seal_slot = self
            .seals
            .binary_search_by_key(&(frame, source), |s| (s.frame, s.source));
        if seal_slot.is_ok() {
            return fail("duplicate source seal");
        }
        let slot = self.sources.binary_search_by_key(&source, |s| s.source);
        let mut state = slot
            .ok()
            .map(|i| self.sources[i])
            .unwrap_or(SourceState::new(source));
        if [attempts, lost, admitted, refused].contains(&u64::MAX) {
            return fail("source seal counter exhausted");
        }
        if attempts < state.last
            || lost < state.lost
            || state.seen.checked_add(lost) != Some(attempts)
        {
            return fail("source seal frontier does not reconcile");
        }
        let retained = self
            .admissions
            .iter()
            .filter(|a| a.source == source && a.frame == frame)
            .count() as u64;
        state.uncertain |= lost != 0;
        if admitted < retained || admitted > attempts || (!state.uncertain && admitted != retained)
        {
            return fail("source seal frame count does not reconcile");
        }
        state.last = attempts;
        state.lost = lost;
        let source_total = self.sources.len() + usize::from(slot.is_err());
        let grow_seals = total > self.seals.capacity();
        let grow_sources = source_total > self.sources.capacity();
        let peak = (|| {
            self.result
                .accounted_bytes
                .checked_add(if grow_seals { bytes::<Seal>(total)? } else { 0 })?
                .checked_add(if grow_sources {
                    bytes::<SourceState>(source_total)?
                } else {
                    0
                })
        })();
        if peak.is_none_or(|v| v > self.result.limits.bytes) {
            self.refuse(Limit::Storage, chunk);
            return Ok(());
        }
        let new_seals = if grow_seals {
            let Some(v) = exact::<Seal>(total) else {
                self.refuse(Limit::Allocation, chunk);
                return Ok(());
            };
            Some(v)
        } else {
            None
        };
        let new_sources = if grow_sources {
            let Some(v) = exact::<SourceState>(source_total) else {
                self.refuse(Limit::Allocation, chunk);
                return Ok(());
            };
            Some(v)
        } else {
            None
        };
        if let Some(mut v) = new_seals {
            v.append(&mut self.seals);
            self.seals = v;
        }
        if let Some(mut v) = new_sources {
            v.append(&mut self.sources);
            self.sources = v;
        }
        self.seals.insert(
            seal_slot.unwrap_err(),
            Seal {
                source,
                frame,
                chunk,
                close_chunk,
                admitted,
                refused,
                admission_unknown: state.uncertain,
                membership_unknown: close.state != crate::checkpoints::InventoryState::Verified,
            },
        );
        match slot {
            Ok(i) => self.sources[i] = state,
            Err(i) => self.sources.insert(i, state),
        }
        self.account();
        Ok(())
    }

    fn charge(&mut self, ops: Option<u64>, chunk: u64) -> bool {
        let total = ops.and_then(|n| n.checked_add(self.result.used_ops));
        match total.filter(|&n| n <= self.result.limits.ops) {
            Some(n) => {
                self.result.used_ops = n;
                true
            }
            None => {
                self.refuse(Limit::Work, chunk);
                false
            }
        }
    }
    fn terminal(&mut self, payload: &[u8], chunk: u64, cap: &Capture) -> Result<(), Malformed> {
        if payload.len() < 64 {
            return fail("source terminal payload length");
        }
        let n = u32::from_le_bytes(payload[56..60].try_into().unwrap()) as usize;
        if n.checked_mul(16).and_then(|v| v.checked_add(64)) != Some(payload.len()) {
            return fail("source terminal payload length");
        }
        self.result.stream_seen = true;
        if self.result.refusal.is_some() {
            return Ok(());
        }
        if let Some(r) = cap
            .dependencies
            .as_ref()
            .and_then(|i| i.status.refusal)
            .filter(|r| r.chunk_seq < chunk)
        {
            self.refuse(Limit::Dependencies, r.chunk_seq);
            return Ok(());
        }
        if self
            .entries()
            .checked_add(1)
            .is_none_or(|v| v > self.result.limits.entries)
        {
            self.refuse(Limit::Entries, chunk);
            return Ok(());
        }
        let work = (|| {
            // At most five independent dependency queries per admission. Each
            // has its own 4096 work ceiling; reserve all of it cumulatively.
            (self.admissions.len() as u64)
                .checked_mul(5 * 4096 + 128 * (n as u64 + 1))?
                .checked_add(
                    (self.begins.len() as u64)
                        .checked_mul(64 * (self.admissions.len() as u64 + 1))?,
                )?
                .checked_add(
                    (self.seals.len() as u64
                        + self.sources.len() as u64
                        + self.result.results.len() as u64
                        + n as u64
                        + 1)
                    .checked_mul(128)?,
                )
        })();
        if !self.charge(work, chunk) {
            return Ok(());
        }
        let mut c = Cursor::new(payload, "source terminal");
        if c.u16()? != 1 {
            return fail("source terminal schema");
        }
        let outcome = match c.u8()? {
            1 => Outcome::CompleteRecorded,
            2 => Outcome::NoData,
            3 => Outcome::Partial,
            4 => Outcome::Unknown,
            _ => return fail("source terminal outcome"),
        };
        if c.u8()? != 0 {
            return fail("source terminal reserved field");
        }
        let frame = Frame {
            owner: c.u32()?,
            id: c.u64()?,
        };
        let source = crate::checkpoints::key(&mut c)?;
        let close_chunk = c.u64()?;
        let seal_chunk = c.u64()?;
        let reasons = c.u64()?;
        c.u32()?;
        if c.u32()? != 0 || reasons & !1023 != 0 {
            return fail("source terminal reason or reserved bits");
        }
        let Ok(si) = self
            .seals
            .binary_search_by_key(&(frame, source), |s| (s.frame, s.source))
        else {
            return fail("source terminal lacks CLOSE and SEAL");
        };
        let seal = self.seals[si];
        if seal.chunk != seal_chunk || seal.close_chunk != close_chunk || seal_chunk >= chunk {
            return fail("source terminal CLOSE or SEAL identity differs");
        }
        let slot = self
            .result
            .results
            .binary_search_by_key(&(frame, source), |s| (s.frame, s.source));
        if slot.is_ok() {
            return fail("duplicate source terminal");
        }
        let state = self.sources[self
            .sources
            .binary_search_by_key(&source, |s| s.source)
            .expect("seal has source")];
        let unknown = seal.admission_unknown || state.uncertain;
        let mut last = 0;
        for _ in 0..n {
            let id = c.u64()?;
            let end = c.u64()?;
            if id <= last || id == u64::MAX || end == u64::MAX {
                return fail("source terminal resolved identities not sorted or exhausted");
            }
            last = id;
            if !self
                .admissions
                .iter()
                .any(|a| a.source == source && a.frame == frame && a.id == id && a.chunk < chunk)
            {
                return fail(
                    "source terminal resolved admission not present for this source and frame",
                );
            }
        }
        c.done()?;
        if source.kind == 2 && (outcome != Outcome::Unknown || n != 0) {
            return fail("source terminal SPU requires UNKNOWN without resolved pairs");
        }
        if !unknown
            && self.begins.iter().any(|b| {
                b.source == source
                    && b.frame == Some(frame)
                    && b.chunk < chunk
                    && !self
                        .admissions
                        .iter()
                        .any(|a| a.source == source && a.frame == frame && a.begin == b.seq)
            })
        {
            return fail("source terminal reverse BEGIN inventory lacks admission");
        }
        let mut predicates = u64::from(seal.refused > 0)
            | (u64::from(seal.membership_unknown) << 3)
            | (u64::from(unknown) << 4)
            | (u64::from(state.attribution) << 5)
            | (u64::from(source.kind == 2) << 7);
        let mut retained = 0u64;
        for a in self
            .admissions
            .iter()
            .filter(|a| a.source == source && a.frame == frame)
        {
            retained += 1;
            let listed = payload[64..]
                .chunks_exact(16)
                .find(|p| u64::from_le_bytes(p[..8].try_into().unwrap()) == a.id)
                .map(|p| u64::from_le_bytes(p[8..].try_into().unwrap()));
            let resolved = if source.kind == 2 {
                Resolution::Unresolved
            } else {
                resolve(a, cap, chunk)
            };
            if matches!(resolved, Resolution::Policy) {
                self.refuse(Limit::Dependencies, chunk);
                return Ok(());
            }
            if matches!(resolved, Resolution::InvalidIdentity) {
                return fail("source terminal known false dependency identity");
            }
            if let Some(end) = listed {
                if !matches!(resolved,Resolution::Valid(found) if found == end) {
                    return fail("source terminal resolved pair is not valid at this position");
                }
            } else {
                predicates |= 1 << 2;
            }
            predicates |= match resolved {
                Resolution::Valid(_) => 0,
                Resolution::Lost => (1 << 1) | (1 << 2),
                Resolution::Epoch => (1 << 6) | (1 << 2),
                Resolution::Attribution => (1 << 5) | (1 << 2),
                Resolution::Unresolved => 1 << 2,
                Resolution::Policy | Resolution::InvalidIdentity => unreachable!(),
            };
        }
        if reasons & 255 & !predicates != 0 {
            return fail("source terminal reason lacks evidence");
        }
        match outcome {
            Outcome::CompleteRecorded | Outcome::NoData => {
                if predicates & (1 << 5) != 0 {
                    return fail("source terminal attribution unknown");
                }
                if reasons != 0 || predicates != 0 {
                    return fail("source terminal completion has uncertain or omitted work");
                }
                if outcome == Outcome::CompleteRecorded
                    && (retained == 0 || retained != n as u64 || retained != seal.admitted)
                {
                    return fail("source terminal COMPLETE inventory differs");
                }
                if outcome == Outcome::NoData && (retained != 0 || n != 0 || seal.admitted != 0) {
                    return fail("source terminal NO_DATA has admitted work");
                }
            }
            Outcome::Partial if reasons & ((1 << 0) | (1 << 1) | (1 << 8) | (1 << 9)) == 0 => {
                return fail("source terminal PARTIAL lacks known omission")
            }
            Outcome::Unknown if reasons & 1020 == 0 => {
                return fail("source terminal UNKNOWN lacks unavailable premise")
            }
            _ => {}
        }
        let total = self.result.results.len() + 1;
        let peak =
            bytes::<SourceResult>(total).and_then(|v| self.result.accounted_bytes.checked_add(v));
        if peak.is_none_or(|v| v > self.result.limits.bytes) {
            self.refuse(Limit::Storage, chunk);
            return Ok(());
        }
        let Some(mut results) = exact(total) else {
            self.refuse(Limit::Allocation, chunk);
            return Ok(());
        };
        results.append(&mut self.result.results);
        results.insert(
            slot.unwrap_err(),
            SourceResult {
                frame,
                source,
                outcome,
                reasons,
                observed_reasons: predicates,
                close_chunk,
                seal_chunk,
                terminal_chunk: chunk,
                resolved: n as u64,
            },
        );
        self.result.results = results;
        self.account();
        Ok(())
    }
    fn entries(&self) -> u64 {
        self.admissions.len() as u64
            + self.seals.len() as u64
            + self.begins.len() as u64
            + self.result.results.len() as u64
    }
    pub(crate) fn finish(
        mut self,
        membership: Option<&crate::checkpoints::Reader>,
        cap: &Capture,
        chunk: u64,
    ) -> Evidence {
        if !self.result.stream_seen || self.result.refusal.is_some() {
            return self.result;
        }
        if let Some(r) = cap
            .dependencies
            .as_ref()
            .and_then(|i| i.status.refusal)
            .filter(|r| r.chunk_seq < chunk)
        {
            self.refuse(Limit::Dependencies, r.chunk_seq);
            return self.result;
        }
        let Some(membership_reader) = membership.filter(|m| m.result.refusal.is_none()) else {
            self.refuse(Limit::Membership, chunk);
            return self.result;
        };
        let membership = &membership_reader.result;
        // Precharge the size-only closure scan before traversing it.
        if !self.charge(
            (membership.closed.len() as u64)
                .checked_add(cap.lanes.len() as u64)
                .and_then(|v| v.checked_add(1))
                .and_then(|v| v.checked_mul(64)),
            chunk,
        ) {
            return self.result;
        }
        let frames = cap
            .lanes
            .values()
            .try_fold(0u64, |n, l| n.checked_add(l.frames.len() as u64));
        let n = frames
            .and_then(|n| n.checked_mul(membership_reader.participant_width()))
            .and_then(|n| n.checked_add(self.admissions.len() as u64))
            .and_then(|n| n.checked_add(self.seals.len() as u64))
            .and_then(|n| n.checked_add(self.begins.len() as u64))
            .and_then(|n| {
                membership
                    .closed
                    .values()
                    .try_fold(n, |n, c| n.checked_add(c.members.len() as u64))
            });
        let Some(n) = n.filter(|&n| {
            n <= usize::MAX as u64
                && self
                    .entries()
                    .checked_add(n)
                    .is_some_and(|v| v <= self.result.limits.entries)
        }) else {
            self.refuse(Limit::Entries, chunk);
            return self.result;
        };
        let work = n.checked_mul(256).and_then(|v| {
            v.checked_add(self.seals.len() as u64 * 64 + self.result.results.len() as u64 * 64)
        });
        if !self.charge(work, chunk) {
            return self.result;
        }
        let peak = (|| {
            self.result
                .accounted_bytes
                .checked_add(bytes::<(Frame, Source)>(n as usize)?)?
                .checked_add(bytes::<Pending>(n as usize)?)
        })();
        if peak.is_none_or(|v| v > self.result.limits.bytes) {
            self.refuse(Limit::Storage, chunk);
            return self.result;
        }
        let Some(mut keys) = exact::<(Frame, Source)>(n as usize) else {
            self.refuse(Limit::Allocation, chunk);
            return self.result;
        };
        let Some(mut pending) = exact::<Pending>(n as usize) else {
            self.refuse(Limit::Allocation, chunk);
            return self.result;
        };
        keys.extend(self.admissions.iter().map(|a| (a.frame, a.source)));
        keys.extend(self.seals.iter().map(|s| (s.frame, s.source)));
        keys.extend(
            self.begins
                .iter()
                .filter_map(|b| b.frame.map(|f| (f, b.source))),
        );
        membership_reader.visit_participants(cap, |f, s| keys.push((f, s)));
        for close in membership.closed.values() {
            keys.extend(close.members.iter().map(|&s| (close.frame, s)));
        }
        keys.sort_unstable();
        keys.dedup();
        for (frame, source) in keys {
            if self
                .result
                .results
                .binary_search_by_key(&(frame, source), |r| (r.frame, r.source))
                .is_ok()
            {
                continue;
            }
            let missing = if !membership.closed.contains_key(&frame) {
                "missing CLOSE"
            } else if membership.closed[&frame]
                .members
                .binary_search(&source)
                .is_err()
            {
                "unrostered source; pending membership"
            } else if self
                .seals
                .binary_search_by_key(&(frame, source), |s| (s.frame, s.source))
                .is_err()
            {
                "missing SOURCE_SEAL"
            } else {
                "missing SOURCE_CHECKPOINT"
            };
            pending.push(Pending {
                frame,
                source,
                missing,
            });
        }
        self.result.accounted_bytes += bytes::<Pending>(pending.capacity()).unwrap();
        self.result.pending = pending;
        self.result
    }
    /// Called only after a membership record has been accepted.
    pub(crate) fn participate(&mut self, source: Source, chunk: u64) {
        if self.result.refusal.is_some() {
            return;
        }
        if !self.charge(
            (self.sources.len() as u64)
                .checked_add(1)
                .and_then(|v| v.checked_mul(64)),
            chunk,
        ) {
            return;
        }
        match self.sources.binary_search_by_key(&source, |s| s.source) {
            Ok(i) => self.sources[i].participating = true,
            Err(i) => {
                let n = self.sources.len() + 1;
                let peak = bytes::<SourceState>(n)
                    .and_then(|v| v.checked_add(self.result.accounted_bytes));
                if peak.is_none_or(|v| v > self.result.limits.bytes) {
                    self.refuse(Limit::Storage, chunk);
                    return;
                }
                let Some(mut replacement) = exact(n) else {
                    self.refuse(Limit::Allocation, chunk);
                    return;
                };
                replacement.append(&mut self.sources);
                let mut state = SourceState::new(source);
                state.participating = true;
                replacement.insert(i, state);
                self.sources = replacement;
                self.account();
            }
        }
    }
    /// Validate the accepted legacy batch before either evidence index commits.
    /// Staging copies include all live capacities in their peak accounting.
    pub(crate) fn observe(
        &mut self,
        batch: Option<&crate::dependencies::Batch>,
        chunk: u64,
    ) -> Result<(), Malformed> {
        if self.result.refusal.is_some() {
            return Ok(());
        }
        let Some(batch) = batch else {
            self.refuse(Limit::Dependencies, chunk);
            return Ok(());
        };
        let n = batch.entries.len();
        let work = (|| {
            (self.sources.len() as u64)
                .checked_add(self.admissions.len() as u64)?
                .checked_add(self.seals.len() as u64)?
                .checked_add(self.begins.len() as u64)?
                .checked_add(n as u64)?
                .checked_add(1)?
                .checked_mul(n as u64 + 1)?
                .checked_mul(64)
        })();
        if !self.charge(work, chunk) {
            return Ok(());
        }
        let new_begins = batch
            .entries
            .iter()
            .filter(|e| e.fact.is_some_and(|r| r.kind == 1))
            .count();
        if self
            .entries()
            .checked_add(new_begins as u64)
            .is_none_or(|v| v > self.result.limits.entries)
        {
            self.refuse(Limit::Entries, chunk);
            return Ok(());
        }
        let ns = self.sources.len() + 1;
        let nb = self.begins.len() + new_begins;
        let peak = (|| {
            self.result
                .accounted_bytes
                .checked_add(bytes::<SourceState>(ns)?)?
                .checked_add(bytes::<Begin>(nb)?)
        })();
        if peak.is_none_or(|v| v > self.result.limits.bytes) {
            self.refuse(Limit::Storage, chunk);
            return Ok(());
        }
        let Some(mut states) = exact::<SourceState>(ns) else {
            self.refuse(Limit::Allocation, chunk);
            return Ok(());
        };
        let Some(mut begins) = exact::<Begin>(nb) else {
            self.refuse(Limit::Allocation, chunk);
            return Ok(());
        };
        states.extend_from_slice(&self.sources);
        begins.extend_from_slice(&self.begins);
        let si = match states.binary_search_by_key(&batch.source, |s| s.source) {
            Ok(i) => i,
            Err(i) => {
                states.insert(i, SourceState::new(batch.source));
                i
            }
        };
        for (i, entry) in batch.entries.iter().enumerate() {
            let Some(r) = entry.fact else {
                if states[si].participating {
                    states[si].attribution = true;
                }
                continue;
            };
            if batch.source.kind == 1 && r.kind == crate::model::KIND_FRAME {
                states[si].participating = true;
            }
            if r.kind != 1 {
                if self
                    .admissions
                    .iter()
                    .any(|a| a.source == batch.source && a.begin == r.id.seq)
                {
                    return fail("admission identity names a known non-BEGIN record");
                }
                let end_kind = if batch.source.kind == 1 {
                    crate::model::KIND_END
                } else {
                    2
                };
                if r.kind == end_kind
                    && states[si].participating
                    && !batch.pairs.iter().any(|p| p.end == r.id)
                {
                    states[si].attribution = true;
                }
                continue;
            }
            let frame = if batch.source.kind == 1 {
                batch
                    .entries
                    .get(i + 1)
                    .and_then(|e| e.fact)
                    .and_then(|p| p.token)
            } else {
                r.token
            };
            let epoch = r.epoch.unwrap_or(0);
            let admitted = self
                .admissions
                .iter()
                .find(|a| a.source == batch.source && a.begin == r.id.seq);
            if let Some(a) = admitted {
                if (batch.source.kind != 2 && frame.is_some_and(|f| f != a.frame))
                    || epoch != a.epoch
                {
                    return fail("recorded BEGIN token or epoch contradicts admission");
                }
            } else if frame.is_some_and(|f| {
                self.seals
                    .iter()
                    .any(|s| s.frame == f && s.source == batch.source && !s.admission_unknown)
            }) {
                return fail("recorded BEGIN absent from known sealed admission inventory");
            }
            if frame.is_none() && states[si].participating {
                states[si].attribution = true;
            }
            begins.push(Begin {
                source: batch.source,
                frame,
                seq: r.id.seq,
                epoch,
                chunk,
            });
        }
        // There are no fallible operations after this publication point.
        self.sources = states;
        self.begins = begins;
        self.account();
        Ok(())
    }
}

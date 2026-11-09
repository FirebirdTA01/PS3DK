//! Accepted record identities. Presence is not frame completion or valid timing.
use crate::checkpoints::{Frame, Source};

// Tests observe actual publication, including the path that later returns Err.
// This is deliberately absent from production builds.
#[cfg(test)]
std::thread_local! {
    pub(crate) static COMMITTED: std::cell::RefCell<Vec<(u64, usize)>> = const { std::cell::RefCell::new(Vec::new()) };
}
#[cfg(test)]
std::thread_local! { pub(crate) static FAIL_NEXT_COMMIT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Position {
    pub chunk_seq: u64,
    pub item_index: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct RecordId {
    pub source: Source,
    pub seq: u64,
}
#[derive(Debug, Clone, Copy, Default)]
pub struct Expected {
    pub epoch: Option<u32>,
    pub kind: Option<u8>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DependencyBudgets {
    pub entries: u64,
    pub bytes: u64,
    pub ops: u64,
}
impl Default for DependencyBudgets {
    fn default() -> Self {
        Self {
            entries: 262144,
            bytes: 64 * 1024 * 1024,
            ops: 100_000_000,
        }
    }
}
#[derive(Debug, Clone, Copy)]
pub struct QueryBudget {
    pub ops: u64,
}
impl Default for QueryBudget {
    fn default() -> Self {
        Self { ops: 4096 }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnavailableReason {
    WrongEpoch,
    WrongKind,
    HistoryNotIndexed,
    IndexRefused,
    QueryBudget,
    PairNotEstablished,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Lookup<T> {
    Present(T),
    Lost { first: u64, next: u64, position: Position },
    NotYetRecorded,
    Unavailable(UnavailableReason),
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecordFact {
    pub id: RecordId,
    pub position: Position,
    pub epoch: Option<u32>,
    pub kind: u8,
    pub depth: u8,
    pub descriptor: Option<u32>,
    pub span_id: Option<u64>,
    pub token: Option<Frame>,
    pub parent_span_id: Option<u64>,
    pub frame_id: Option<u64>,
    pub flags: u16,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PairFact {
    pub begin: RecordId,
    pub parent: Option<RecordId>,
    pub end: RecordId,
    pub position: Position,
    pub truncated_at_stop: bool,
    pub affected_at_match: bool,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RefusalReason {
    Entries,
    Storage,
    Work,
    Allocation,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Refusal {
    pub reason: RefusalReason,
    pub chunk_seq: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IndexStatus {
    pub records: u64,
    pub pairs: u64,
    pub accounted_bytes: u64,
    pub used_ops: u64,
    pub limits: DependencyBudgets,
    pub refusal: Option<Refusal>,
}
/// Shared CLI and viewer explanation; indexed presence is not completion.
pub fn status_text(status: &IndexStatus) -> String {
    let state = match status.refusal {
        Some(r) => format!(
            "INCOMPLETE ({} at chunk {})",
            match r.reason {
                RefusalReason::Entries => "entry limit",
                RefusalReason::Storage => "storage limit",
                RefusalReason::Work => "work limit",
                RefusalReason::Allocation => "allocation refusal",
            },
            r.chunk_seq
        ),
        None => "INDEXED".to_owned(),
    };
    format!("Dependency evidence: {state}; retained records {}, pairs {}; accounted bytes {}, used work {}; PC policy ceiling entries {}, bytes {}, work {}. Missing indexed evidence is unavailable; indexed presence does not establish completion.",
        status.records, status.pairs, status.accounted_bytes, status.used_ops,
        status.limits.entries, status.limits.bytes, status.limits.ops)
}
impl UnavailableReason {
    pub fn text(self) -> &'static str {
        match self {
            Self::WrongEpoch => "UNAVAILABLE (epoch mismatch)",
            Self::WrongKind => "UNAVAILABLE (record kind mismatch)",
            Self::HistoryNotIndexed => "UNAVAILABLE (historical diagnostics not indexed)",
            Self::IndexRefused => "UNAVAILABLE (dependency ingestion refused)",
            Self::QueryBudget => "UNAVAILABLE (query work limit)",
            Self::PairNotEstablished => "UNAVAILABLE (pair not established)",
        }
    }
}
#[derive(Debug, Clone, Copy)]
pub(crate) struct Entry {
    pub first: u64,
    pub next: u64,
    pub position: Position,
    pub fact: Option<RecordFact>,
}
impl Entry {
    pub fn record(fact: RecordFact) -> Option<Self> {
        Some(Self {
            first: fact.id.seq,
            next: fact.id.seq.checked_add(1)?,
            position: fact.position,
            fact: Some(fact),
        })
    }
    pub fn loss(first: u64, count: u64, position: Position) -> Option<Self> {
        if count == 0 {
            return None;
        }
        Some(Self {
            first,
            next: first.checked_add(count)?,
            position,
            fact: None,
        })
    }
}
#[derive(Debug)]
pub(crate) struct Batch {
    pub source: Source,
    pub chunk_seq: u64,
    pub entries: Vec<Entry>,
    pub pairs: Vec<PairFact>,
    reserved_records: usize,
    reserved_pairs: usize,
}
impl Batch {
    pub fn foreign_record(&mut self, event: &crate::ext::RawEvent, item_index: u32) {
        self.entries.push(
            Entry::record(RecordFact {
                id: RecordId {
                    source: self.source,
                    seq: event.seq,
                },
                position: Position {
                    chunk_seq: self.chunk_seq,
                    item_index,
                },
                epoch: Some(event.epoch),
                kind: event.kind,
                depth: event.depth,
                descriptor: Some(event.desc),
                span_id: None,
                token: (event.frame_owner != 0).then_some(Frame {
                    owner: event.frame_owner,
                    id: event.frame_id,
                }),
                parent_span_id: None,
                frame_id: None,
                flags: 0,
            })
            .expect("validated extension tiling range"),
        );
    }
    pub fn foreign_pair(&mut self, pair: &crate::ext::Pair, item_index: u32) {
        self.pairs.push(PairFact {
            begin: RecordId {
                source: self.source,
                seq: pair.begin,
            },
            parent: None,
            end: RecordId {
                source: self.source,
                seq: pair.end,
            },
            position: Position {
                chunk_seq: self.chunk_seq,
                item_index,
            },
            truncated_at_stop: false,
            affected_at_match: false,
        });
    }
    /// Called only for an accepted PPU record. The enclosing chunk still owns
    /// this provisional batch until all of its records have passed validation.
    pub fn ppu_record(&mut self, r: &crate::reader::RawRecord, item_index: u32) {
        use crate::model::*;
        let position = Position {
            chunk_seq: self.chunk_seq,
            item_index,
        };
        let entry = if r.kind == KIND_GAP {
            Entry::loss(r.b, r.c - r.b + 1, position)
        } else {
            Entry::record(RecordFact {
                id: RecordId {
                    source: self.source,
                    seq: r.seq,
                },
                position,
                epoch: None,
                kind: r.kind,
                depth: r.depth,
                flags: r.flags,
                descriptor: matches!(r.kind, KIND_BEGIN | KIND_END).then_some(r.a),
                span_id: matches!(r.kind, KIND_BEGIN | KIND_END).then_some(r.c),
                token: (r.kind == KIND_BEGIN_PARENT && r.a != 0).then_some(Frame { owner: r.a, id: r.c }),
                parent_span_id: (r.kind == KIND_BEGIN_PARENT).then_some(r.b),
                frame_id: (r.kind == KIND_FRAME).then_some(r.c),
            })
        };
        self.entries.push(entry.expect("validated PPU sequence range"));
    }
    pub fn ppu_pair(&mut self, span: &crate::model::Span, end: &crate::reader::RawRecord, item_index: u32) {
        self.pairs.push(PairFact {
            begin: RecordId {
                source: self.source,
                seq: span.begin_seq,
            },
            parent: Some(RecordId {
                source: self.source,
                seq: span.begin_seq + 1,
            }),
            end: RecordId {
                source: self.source,
                seq: end.seq,
            },
            position: Position {
                chunk_seq: self.chunk_seq,
                item_index,
            },
            truncated_at_stop: span.truncated_at_stop,
            affected_at_match: span.affected,
        });
    }
}
#[derive(Debug, Clone)]
pub(crate) struct DependencyIndex {
    pub status: IndexStatus,
    sources: Vec<(Source, Vec<Entry>)>,
    pairs: Vec<PairFact>,
}
impl DependencyIndex {
    pub fn begin_counted(
        &mut self,
        source: Source,
        chunk_seq: u64,
        records: usize,
        count_pairs: impl FnOnce() -> usize,
    ) -> Option<Batch> {
        if self.status.refusal.is_some() {
            return None;
        }
        if self
            .status
            .records
            .checked_add(self.status.pairs)
            .and_then(|n| n.checked_add(records as u64))
            .is_none_or(|n| n > self.status.limits.entries)
        {
            self.refuse(RefusalReason::Entries, chunk_seq);
            return None;
        }
        let Some(ops) = self
            .status
            .used_ops
            .checked_add(records as u64)
            .filter(|&n| n <= self.status.limits.ops)
        else {
            self.refuse(RefusalReason::Work, chunk_seq);
            return None;
        };
        self.status.used_ops = ops;
        let pairs = count_pairs();
        self.begin(source, chunk_seq, records, pairs)
    }
    pub fn new(limits: DependencyBudgets) -> Self {
        Self {
            status: IndexStatus {
                records: 0,
                pairs: 0,
                accounted_bytes: 0,
                used_ops: 0,
                limits,
                refusal: None,
            },
            sources: Vec::new(),
            pairs: Vec::new(),
        }
    }
    pub fn begin(&mut self, source: Source, chunk_seq: u64, records: usize, pairs: usize) -> Option<Batch> {
        if self.status.refusal.is_some() {
            return None;
        }
        let units = (records as u64).checked_add(pairs as u64);
        if units
            .and_then(|n| n.checked_add(self.status.records)?.checked_add(self.status.pairs))
            .is_none_or(|n| n > self.status.limits.entries)
        {
            self.refuse(RefusalReason::Entries, chunk_seq);
            return None;
        }
        let Some(work) = units
            .and_then(|n| n.checked_mul(4))
            .and_then(|n| n.checked_add(search_charge(self.sources.len())))
        else {
            self.refuse(RefusalReason::Work, chunk_seq);
            return None;
        };
        if self
            .status
            .used_ops
            .checked_add(work)
            .is_none_or(|n| n > self.status.limits.ops)
        {
            self.refuse(RefusalReason::Work, chunk_seq);
            return None;
        }
        self.status.used_ops += work;
        let slot = self.sources.binary_search_by_key(&source, |s| s.0);
        let old = slot.ok().map(|n| &self.sources[n].1);
        let old_len = old.map_or(0, Vec::len);
        let old_cap = old.map_or(0, Vec::capacity);
        let required = old_len.checked_add(records);
        let destination = required.map(|n| if n > old_cap { n } else { 0 });
        let new_source = usize::from(old.is_none() && records != 0);
        let source_capacity = self.sources.len().checked_add(new_source);
        let pair_capacity = self.pairs.len().checked_add(pairs);
        // Reserve before copying or shifting retained vectors. Pair insertion
        // is bounded by the full destination length for each new pair.
        let extra_work = self
            .pairs
            .len()
            .checked_add(pairs)
            .and_then(|n| search_charge(n).checked_mul(2)?.checked_add(n as u64))
            .and_then(|n| n.checked_mul(pairs as u64))
            .and_then(|n| {
                n.checked_add(if pair_capacity.is_some_and(|v| v > self.pairs.capacity()) {
                    self.pairs.len() as u64
                } else {
                    0
                })
            })
            .and_then(|n| {
                n.checked_add(if new_source != 0 {
                    self.sources.len() as u64 * 2
                } else {
                    0
                })
            })
            .and_then(|n| {
                n.checked_add(if destination.is_some_and(|n| n != 0) {
                    old_len as u64
                } else {
                    0
                })
            });
        let Some(total_work) = extra_work
            .and_then(|n| self.status.used_ops.checked_add(n))
            .filter(|&n| n <= self.status.limits.ops)
        else {
            self.refuse(RefusalReason::Work, chunk_seq);
            return None;
        };
        self.status.used_ops = total_work;
        // Old capacities, every possible replacement and staging coexist in
        // this peak reservation. There are no opaque map-node allowances.
        let staging = (records as u64)
            .checked_mul(std::mem::size_of::<Entry>() as u64)
            .and_then(|n| n.checked_add((pairs as u64).checked_mul(std::mem::size_of::<PairFact>() as u64)?));
        let peak = destination
            .and_then(|n| (n as u64).checked_mul(std::mem::size_of::<Entry>() as u64))
            .and_then(|n| n.checked_add(staging?))
            .and_then(|n| n.checked_add(growth_bytes::<PairFact>(self.pairs.capacity(), pair_capacity?)?))
            .and_then(|n| {
                n.checked_add(growth_bytes::<(Source, Vec<Entry>)>(
                    self.sources.capacity(),
                    source_capacity?,
                )?)
            })
            .and_then(|n| n.checked_add(self.status.accounted_bytes));
        if peak.is_none_or(|n| n > self.status.limits.bytes) {
            self.refuse(RefusalReason::Storage, chunk_seq);
            return None;
        }
        let (Ok(entries), Ok(matches)) = (exact_vector(records), exact_vector(pairs)) else {
            self.refuse(RefusalReason::Allocation, chunk_seq);
            return None;
        };
        Some(Batch {
            source,
            chunk_seq,
            entries,
            pairs: matches,
            reserved_records: records,
            reserved_pairs: pairs,
        })
    }
    fn refuse(&mut self, reason: RefusalReason, chunk_seq: u64) {
        if self.status.refusal.is_none() {
            self.status.refusal = Some(Refusal { reason, chunk_seq });
        }
    }
    pub fn commit(&mut self, mut batch: Batch) {
        if self.status.refusal.is_some() {
            return;
        }
        #[cfg(test)]
        if FAIL_NEXT_COMMIT.with(|v| v.replace(false)) {
            self.refuse(RefusalReason::Allocation,batch.chunk_seq); return;
        }
        assert!(batch.entries.len() <= batch.reserved_records && batch.pairs.len() <= batch.reserved_pairs);
        assert!(batch.entries.iter().all(|e| e.first < e.next
            && e.position.chunk_seq == batch.chunk_seq
            && e.fact
                .is_none_or(|f| f.id.source == batch.source && f.id.seq == e.first)));
        assert!(batch.entries.windows(2).all(|w| w[0].next <= w[1].first));
        let slot = self.sources.binary_search_by_key(&batch.source, |s| s.0);
        if let (Some(old), Some(first)) = (slot.ok().and_then(|n| self.sources[n].1.last()), batch.entries.first()) {
            assert!(old.next <= first.first);
        }
        assert!(batch.pairs.iter().all(|p| p.begin.source == batch.source
            && p.end.source == batch.source
            && p.position.chunk_seq == batch.chunk_seq
            && self.pairs.binary_search_by_key(&p.begin, |p| p.begin).is_err()));
        if batch.entries.is_empty() && batch.pairs.is_empty() {
            return;
        }
        let n = batch.entries.len();
        let pn = batch.pairs.len();
        // Allocate all replacements before moving any accepted data. A failed
        // allocation therefore cannot publish a partial batch or grow the
        // retained allocation outside the previously reserved accounting.
        let replacement_entries = match slot {
            Ok(i) => replacement(self.sources[i].1.capacity(), self.sources[i].1.len() + n),
            Err(_) => Ok(None),
        };
        let replacements = (
            replacement_entries,
            replacement(
                self.sources.capacity(),
                self.sources.len() + usize::from(slot.is_err() && n != 0),
            ),
            replacement(self.pairs.capacity(), self.pairs.len() + pn),
        );
        let (Ok(entries), Ok(sources), Ok(pairs)) = replacements else {
            self.refuse(RefusalReason::Allocation, batch.chunk_seq);
            return;
        };
        if let Some(mut v) = sources {
            self.status.accounted_bytes +=
                ((v.capacity() - self.sources.capacity()) * std::mem::size_of::<(Source, Vec<Entry>)>()) as u64;
            v.append(&mut self.sources);
            self.sources = v;
        }
        match slot {
            Ok(i) => {
                let dest = &mut self.sources[i].1;
                if let Some(mut v) = entries {
                    self.status.accounted_bytes +=
                        ((v.capacity() - dest.capacity()) * std::mem::size_of::<Entry>()) as u64;
                    v.append(dest);
                    *dest = v;
                }
                dest.append(&mut batch.entries);
            }
            Err(i) if n != 0 => {
                self.status.accounted_bytes += (batch.entries.capacity() * std::mem::size_of::<Entry>()) as u64;
                self.sources.insert(i, (batch.source, batch.entries));
            }
            _ => {}
        }
        if let Some(mut v) = pairs {
            self.status.accounted_bytes +=
                ((v.capacity() - self.pairs.capacity()) * std::mem::size_of::<PairFact>()) as u64;
            v.append(&mut self.pairs);
            self.pairs = v;
        }
        for pair in batch.pairs {
            let pos = self
                .pairs
                .binary_search_by_key(&pair.begin, |p| p.begin)
                .expect_err("unique matching BEGIN");
            self.pairs.insert(pos, pair);
        }
        self.status.records += n as u64;
        self.status.pairs += pn as u64;
        #[cfg(test)]
        COMMITTED.with(|trace| trace.borrow_mut().push((batch.chunk_seq, n)));
    }
    pub fn record(&self, id: RecordId, expected: Expected, before: u64, budget: QueryBudget) -> Lookup<RecordFact> {
        self.record_inner(id, expected, before, &mut Meter(budget.ops))
    }
    fn refused_before(&self, before: u64) -> bool {
        self.status.refusal.is_some_and(|r| r.chunk_seq < before)
    }
    fn missing<T>(&self, before: u64) -> Lookup<T> {
        if self.refused_before(before) {
            Lookup::Unavailable(UnavailableReason::IndexRefused)
        } else {
            Lookup::NotYetRecorded
        }
    }
    fn record_inner(&self, id: RecordId, expected: Expected, before: u64, meter: &mut Meter) -> Lookup<RecordFact> {
        if !meter.take(search_charge(self.sources.len())) {
            return Lookup::Unavailable(UnavailableReason::QueryBudget);
        }
        let Ok(slot) = self.sources.binary_search_by_key(&id.source, |s| s.0) else {
            return self.missing(before);
        };
        let entries = &self.sources[slot].1;
        let (mut lo, mut hi) = (0, entries.len());
        while lo < hi {
            if !meter.take(1) {
                return Lookup::Unavailable(UnavailableReason::QueryBudget);
            }
            let mid = lo + (hi - lo) / 2;
            if entries[mid].next <= id.seq {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if !meter.take(3) {
            return Lookup::Unavailable(UnavailableReason::QueryBudget);
        }
        let Some(e) = entries.get(lo).filter(|e| e.first <= id.seq) else {
            return self.missing(before);
        };
        if e.position.chunk_seq >= before {
            return Lookup::NotYetRecorded;
        }
        let Some(fact) = e.fact else {
            return Lookup::Lost {
                first: e.first,
                next: e.next,
                position: e.position,
            };
        };
        if expected.epoch.is_some_and(|epoch| fact.epoch != Some(epoch)) {
            return Lookup::Unavailable(UnavailableReason::WrongEpoch);
        }
        if expected.kind.is_some_and(|kind| fact.kind != kind) {
            return Lookup::Unavailable(UnavailableReason::WrongKind);
        }
        Lookup::Present(fact)
    }
    pub fn pair(&self, begin: RecordId, expected: Expected, before: u64, budget: QueryBudget) -> Lookup<PairFact> {
        let mut meter = Meter(budget.ops);
        match self.record_inner(begin, expected, before, &mut meter) {
            Lookup::Present(_) => {}
            Lookup::Lost { first, next, position } => return Lookup::Lost { first, next, position },
            Lookup::NotYetRecorded => return Lookup::NotYetRecorded,
            Lookup::Unavailable(why) => return Lookup::Unavailable(why),
        }
        if !meter.take(search_charge(self.pairs.len())) {
            return Lookup::Unavailable(UnavailableReason::QueryBudget);
        }
        let Ok(slot) = self.pairs.binary_search_by_key(&begin, |p| p.begin) else {
            return Lookup::Unavailable(if self.refused_before(before) {
                UnavailableReason::IndexRefused
            } else {
                UnavailableReason::PairNotEstablished
            });
        };
        let pair = &self.pairs[slot];
        if pair.position.chunk_seq >= before {
            return Lookup::NotYetRecorded;
        }
        for id in [pair.parent, Some(pair.end)].into_iter().flatten() {
            match self.record_inner(
                id,
                Expected {
                    epoch: expected.epoch,
                    kind: None,
                },
                before,
                &mut meter,
            ) {
                Lookup::Present(_) => {}
                Lookup::Lost { first, next, position } => return Lookup::Lost { first, next, position },
                Lookup::NotYetRecorded => return Lookup::NotYetRecorded,
                Lookup::Unavailable(why) => return Lookup::Unavailable(why),
            }
        }
        Lookup::Present(*pair)
    }
}

fn growth_bytes<T>(capacity: usize, needed: usize) -> Option<u64> {
    if needed <= capacity {
        Some(0)
    } else {
        (needed as u64).checked_mul(std::mem::size_of::<T>() as u64)
    }
}
fn exact_vector<T>(needed: usize) -> Result<Vec<T>, ()> {
    let mut v = Vec::new();
    v.try_reserve_exact(needed).map_err(|_| ())?;
    // The allocator may provide more bytes internally, which is outside the
    // payload-capacity accounting. Any excess *reported Vec capacity* is refused.
    if v.capacity() != needed {
        return Err(());
    }
    Ok(v)
}
fn replacement<T>(capacity: usize, needed: usize) -> Result<Option<Vec<T>>, ()> {
    if needed <= capacity {
        Ok(None)
    } else {
        exact_vector(needed).map(Some)
    }
}

fn search_charge(n: usize) -> u64 {
    64 * (1 + u64::from(usize::BITS - n.leading_zeros()))
}
struct Meter(u64);
impl Meter {
    fn take(&mut self, n: u64) -> bool {
        if let Some(v) = self.0.checked_sub(n) {
            self.0 = v;
            true
        } else {
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn source(id: u32) -> Source {
        Source { kind: 1, id, domain: 0 }
    }
    fn fact(seq: u64, chunk: u64, kind: u8) -> RecordFact {
        RecordFact {
            id: RecordId { source: source(1), seq },
            position: Position {
                chunk_seq: chunk,
                item_index: 0,
            },
            epoch: None,
            kind,
            depth: 0,
            descriptor: Some(3),
            span_id: Some(7),
            token: Some(Frame { owner: 1, id: 0 }),
            parent_span_id: None,
            frame_id: None,
            flags: 0,
        }
    }
    fn put(i: &mut DependencyIndex, f: RecordFact) {
        let mut b = i.begin(f.id.source, f.position.chunk_seq, 1, 0).expect("admitted");
        b.entries.push(Entry::record(f).unwrap());
        i.commit(b);
    }
    #[test]
    fn identity_epoch_kind_and_position_are_not_counts() {
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        let f = fact(3, 8, 3);
        put(&mut i, f);
        let q = QueryBudget::default();
        let e = Expected::default();
        assert_eq!(i.record(f.id, e, 9, q), Lookup::Present(f));
        assert_eq!(i.record(f.id, e, 8, q), Lookup::NotYetRecorded);
        assert_eq!(
            i.record(
                RecordId {
                    source: source(2),
                    seq: 3
                },
                e,
                9,
                q
            ),
            Lookup::NotYetRecorded
        );
        assert_eq!(i.record(RecordId { seq: 2, ..f.id }, e, 9, q), Lookup::NotYetRecorded);
        assert_eq!(
            i.record(
                f.id,
                Expected {
                    epoch: Some(1),
                    kind: None
                },
                9,
                q
            ),
            Lookup::Unavailable(UnavailableReason::WrongEpoch)
        );
        assert_eq!(
            i.record(
                f.id,
                Expected {
                    epoch: None,
                    kind: Some(1)
                },
                9,
                q
            ),
            Lookup::Unavailable(UnavailableReason::WrongKind)
        );
    }
    #[test]
    fn loss_is_one_interval_never_an_event() {
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        let p = Position {
            chunk_seq: 4,
            item_index: 0,
        };
        let mut b = i.begin(source(1), 4, 1, 0).unwrap();
        b.entries.push(Entry::loss(5, u64::MAX - 5, p).unwrap());
        i.commit(b);
        for seq in [5, u64::MAX - 1] {
            assert_eq!(
                i.record(
                    RecordId { source: source(1), seq },
                    Expected::default(),
                    5,
                    QueryBudget::default()
                ),
                Lookup::Lost {
                    first: 5,
                    next: u64::MAX,
                    position: p
                }
            );
        }
        assert_eq!(i.status.records, 1);
        assert!(Entry::loss(u64::MAX, 1, p).is_none());
        assert!(Entry::loss(5, 0, p).is_none());
        assert_eq!(
            i.record(
                RecordId {
                    source: source(1),
                    seq: u64::MAX
                },
                Expected::default(),
                5,
                QueryBudget::default()
            ),
            Lookup::NotYetRecorded
        );
    }
    #[test]
    fn pair_requires_all_endpoints_before_the_query() {
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        let a = fact(0, 1, 1);
        let p = fact(1, 1, 2);
        put(&mut i, a);
        put(&mut i, p);
        let z = fact(3, 4, 3);
        let pair = PairFact {
            begin: a.id,
            parent: Some(p.id),
            end: z.id,
            position: z.position,
            truncated_at_stop: true,
            affected_at_match: false,
        };
        let mut b = i.begin(source(1), 4, 1, 1).unwrap();
        b.entries.push(Entry::record(z).unwrap());
        b.pairs.push(pair);
        i.commit(b);
        assert_eq!(
            i.pair(a.id, Expected::default(), 4, QueryBudget::default()),
            Lookup::NotYetRecorded
        );
        assert_eq!(
            i.pair(a.id, Expected::default(), 5, QueryBudget::default()),
            Lookup::Present(pair)
        );
        assert_eq!(
            i.pair(p.id, Expected::default(), 5, QueryBudget::default()),
            Lookup::Unavailable(UnavailableReason::PairNotEstablished)
        );
    }
    #[test]
    fn dropped_batch_publishes_nothing() {
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        let f = fact(0, 1, 1);
        let mut b = i.begin(source(1), 1, 1, 0).unwrap();
        b.entries.push(Entry::record(f).unwrap());
        drop(b);
        assert_eq!(i.status.records, 0);
        assert!(i.sources.is_empty());
        assert_eq!(
            i.record(f.id, Expected::default(), 2, QueryBudget::default()),
            Lookup::NotYetRecorded
        );
    }
    #[test]
    fn refusal_is_sticky_and_does_not_erase_old_entries() {
        let mut i = DependencyIndex::new(DependencyBudgets {
            entries: 1,
            ..DependencyBudgets::default()
        });
        let f = fact(0, 1, 1);
        put(&mut i, f);
        assert!(i.begin(source(1), 2, 1, 0).is_none());
        assert_eq!(
            i.status.refusal,
            Some(Refusal {
                reason: RefusalReason::Entries,
                chunk_seq: 2
            })
        );
        assert_eq!(
            i.record(f.id, Expected::default(), 3, QueryBudget::default()),
            Lookup::Present(f)
        );
        assert_eq!(
            i.record(
                RecordId { seq: 1, ..f.id },
                Expected::default(),
                3,
                QueryBudget::default()
            ),
            Lookup::Unavailable(UnavailableReason::IndexRefused)
        );
        assert!(i.begin(source(2), 3, 0, 0).is_none());
        assert_eq!(i.status.refusal.unwrap().chunk_seq, 2);
    }
    #[test]
    fn query_exhaustion_never_poisons_ingestion_or_next_query() {
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        let f = fact(0, 1, 1);
        put(&mut i, f);
        let before = i.status.clone();
        assert_eq!(
            i.record(f.id, Expected::default(), 2, QueryBudget { ops: 0 }),
            Lookup::Unavailable(UnavailableReason::QueryBudget)
        );
        assert_eq!(i.status, before);
        assert_eq!(
            i.record(f.id, Expected::default(), 2, QueryBudget::default()),
            Lookup::Present(f)
        );
        put(&mut i, fact(1, 2, 3));
        assert_eq!(i.status.records, 2);
    }
    #[test]
    fn zero_and_overflow_reservations_are_visible_refusals() {
        for limits in [
            DependencyBudgets {
                entries: 0,
                ..DependencyBudgets::default()
            },
            DependencyBudgets {
                bytes: 0,
                ..DependencyBudgets::default()
            },
            DependencyBudgets {
                ops: 0,
                ..DependencyBudgets::default()
            },
        ] {
            let mut i = DependencyIndex::new(limits);
            assert!(i.begin(source(1), 9, 1, 0).is_none());
            assert!(i.status.refusal.is_some());
            assert!(i.sources.is_empty());
        }
        let mut i = DependencyIndex::new(DependencyBudgets::default());
        assert!(i.begin(source(1), 9, usize::MAX, usize::MAX).is_none());
        assert!(i.status.refusal.is_some());
    }
    #[test]
    fn exact_ingestion_and_query_limits_have_one_short_controls() {
        let bytes = std::mem::size_of::<(Source, Vec<Entry>)>() as u64 + 2 * std::mem::size_of::<Entry>() as u64;
        let limits = DependencyBudgets {
            entries: 1,
            bytes,
            ops: 68,
        };
        let mut i = DependencyIndex::new(limits);
        let f = fact(0, 1, 1);
        put(&mut i, f);
        assert!(i.status.refusal.is_none());
        assert_eq!(i.status.used_ops, 68);
        assert_eq!(
            i.status.accounted_bytes,
            (std::mem::size_of::<(Source, Vec<Entry>)>() + std::mem::size_of::<Entry>()) as u64
        );
        assert_eq!(
            i.record(f.id, Expected::default(), 2, QueryBudget { ops: 132 }),
            Lookup::Present(f)
        );
        assert_eq!(
            i.record(f.id, Expected::default(), 2, QueryBudget { ops: 131 }),
            Lookup::Unavailable(UnavailableReason::QueryBudget)
        );
        for (l, reason) in [
            (
                DependencyBudgets {
                    bytes: bytes - 1,
                    ..limits
                },
                RefusalReason::Storage,
            ),
            (DependencyBudgets { ops: 67, ..limits }, RefusalReason::Work),
        ] {
            let mut j = DependencyIndex::new(l);
            assert!(j.begin(source(1), 1, 1, 0).is_none());
            assert_eq!(j.status.refusal.unwrap().reason, reason);
            assert!(j.sources.is_empty());
        }
    }
    #[test]
    fn arithmetic_overflow_is_never_an_unlabelled_omission() {
        let limits = DependencyBudgets {
            entries: u64::MAX,
            bytes: u64::MAX,
            ops: u64::MAX,
        };
        let mut i = DependencyIndex::new(limits);
        assert!(i.begin(source(1), 5, usize::MAX / 4 + 1, 0).is_none());
        assert_eq!(
            i.status.refusal,
            Some(Refusal {
                reason: RefusalReason::Work,
                chunk_seq: 5
            })
        );
        let mut i = DependencyIndex::new(limits);
        assert!(i
            .begin(source(1), 6, usize::MAX / std::mem::size_of::<Entry>() + 1, 0)
            .is_none());
        assert_eq!(
            i.status.refusal,
            Some(Refusal {
                reason: RefusalReason::Storage,
                chunk_seq: 6
            })
        );
    }
    #[test]
    fn growing_vectors_charge_retained_copy_work_before_allocation() {
        for (ops, accept) in [(200, false), (201, true)] {
            let mut i = DependencyIndex::new(DependencyBudgets {
                ops,
                ..DependencyBudgets::default()
            });
            put(&mut i, fact(0, 1, 6));
            let b = i.begin(source(1), 2, 1, 0);
            assert_eq!(b.is_some(), accept);
            if !accept {
                assert_eq!(
                    i.status.refusal,
                    Some(Refusal {
                        reason: RefusalReason::Work,
                        chunk_seq: 2
                    })
                );
            }
            assert_eq!(i.status.records, 1);
        }
    }
    #[test]
    fn prescan_is_not_called_without_budget_or_after_refusal() {
        for limits in [
            DependencyBudgets {
                ops: 0,
                ..DependencyBudgets::default()
            },
            DependencyBudgets {
                entries: 0,
                ..DependencyBudgets::default()
            },
        ] {
            let mut index = DependencyIndex::new(limits);
            let calls = std::cell::Cell::new(0);
            assert!(index
                .begin_counted(source(1), 3, 2, || {
                    calls.set(calls.get() + 1);
                    1
                })
                .is_none());
            assert!(index.status.refusal.is_some());
            assert!(index
                .begin_counted(source(1), 4, 2, || {
                    calls.set(calls.get() + 1);
                    1
                })
                .is_none());
            assert_eq!(calls.get(), 0, "no index-only scan without prior admission");
        }
    }
    #[test]
    fn later_ingestion_refusal_does_not_rewrite_earlier_missing_queries() {
        let mut index = DependencyIndex::new(DependencyBudgets {
            entries: 1,
            ..DependencyBudgets::default()
        });
        let f = fact(0, 2, 1);
        put(&mut index, f);
        let absent = RecordId { seq: 99, ..f.id };
        let missing = index.record(absent, Expected::default(), 3, QueryBudget::default());
        let missing_pair = index.pair(f.id, Expected::default(), 3, QueryBudget::default());
        assert!(index.begin(source(1), 5, 1, 0).is_none());
        assert_eq!(
            index.record(absent, Expected::default(), 3, QueryBudget::default()),
            missing
        );
        assert_eq!(
            index.pair(f.id, Expected::default(), 3, QueryBudget::default()),
            missing_pair
        );
        assert_eq!(
            index.record(absent, Expected::default(), 5, QueryBudget::default()),
            missing
        );
        assert_eq!(
            index.record(absent, Expected::default(), 6, QueryBudget::default()),
            Lookup::Unavailable(UnavailableReason::IndexRefused)
        );
    }
    #[test]
    fn first_pair_and_each_growth_account_all_vector_capacities() {
        let e = std::mem::size_of::<Entry>() as u64;
        let p = std::mem::size_of::<PairFact>() as u64;
        let s = std::mem::size_of::<(Source, Vec<Entry>)>() as u64;
        let first_peak = s + 4 * e + 2 * p;
        for (bytes, accepted) in [(first_peak - 1, false), (first_peak, true)] {
            let mut index = DependencyIndex::new(DependencyBudgets {
                bytes,
                ..DependencyBudgets::default()
            });
            let batch = index.begin(source(1), 1, 2, 1);
            assert_eq!(batch.is_some(), accepted, "first pair peak {first_peak}");
        }
        let mut index = DependencyIndex::new(DependencyBudgets::default());
        for i in 0..24u64 {
            let mut b = index.begin(source(1), i + 1, 2, 1).unwrap();
            let begin = fact(2 * i, i + 1, 1);
            let end = fact(2 * i + 1, i + 1, 3);
            b.entries
                .extend([Entry::record(begin).unwrap(), Entry::record(end).unwrap()]);
            b.pairs.push(PairFact {
                begin: begin.id,
                parent: None,
                end: end.id,
                position: end.position,
                truncated_at_stop: false,
                affected_at_match: false,
            });
            index.commit(b);
            let actual = index.sources.capacity() as u64 * s
                + index.pairs.capacity() as u64 * p
                + index.sources.iter().map(|(_, v)| v.capacity() as u64 * e).sum::<u64>();
            assert_eq!(index.status.accounted_bytes, actual, "growth {i}");
            assert_eq!(present_pair(&index, begin.id).end, end.id);
        }
        fn present_pair(index: &DependencyIndex, id: RecordId) -> PairFact {
            match index.pair(id, Expected::default(), 99, QueryBudget::default()) {
                Lookup::Present(p) => p,
                x => panic!("{x:?}"),
            }
        }
        println!("host layout Entry={e} PairFact={p} SourceSlot={s}; all retained Vec capacities accounted");
    }

    #[test]
    fn parser_commit_trace_excludes_a_semantically_failed_chunk() {
        // Small byte fixture for the actual structural parser; the integration
        // suite uses its separate encoder for the wider grammar/CRC coverage.
        fn chunk(out: &mut Vec<u8>, ty: u32, seq: u64, payload: &[u8]) {
            let start = out.len();
            out.extend(ty.to_le_bytes());
            out.extend((payload.len() as u32).to_le_bytes());
            out.extend(seq.to_le_bytes());
            out.extend(payload);
            let crc = crate::hash::crc32(&out[start..]);
            out.extend(crc.to_le_bytes());
        }
        fn records(first: u64, bad: Option<u8>) -> Vec<u8> {
            let mut p = Vec::new();
            p.extend(1u32.to_le_bytes());
            p.extend((if bad.is_some() { 2u32 } else { 1 }).to_le_bytes());
            for (i, kind) in std::iter::once(6u8).chain(bad).enumerate() {
                p.extend([kind, 0, 0, 0]);
                p.extend(1u32.to_le_bytes());
                p.extend((first + i as u64).to_le_bytes());
                p.extend(0u64.to_le_bytes());
                p.extend(0u64.to_le_bytes());
            }
            p
        }
        let mut bytes = b"PS3DKPRF".to_vec();
        bytes.extend(1u16.to_le_bytes());
        bytes.extend(0u16.to_le_bytes());
        bytes.extend(8u32.to_le_bytes());
        let mut h = vec![0; 12]; // six empty length-prefixed strings
        h.extend([1, 1, 0, 0]);
        h.extend(1u64.to_le_bytes());
        for n in [8u32, 8, 8] {
            h.extend(n.to_le_bytes());
        }
        chunk(&mut bytes, 1, 0, &h);
        let mut clock = Vec::new();
        for n in [1u32, 1, 0] {
            clock.extend(n.to_le_bytes());
        }
        clock.extend(79_800_000u64.to_le_bytes());
        clock.extend(0u64.to_le_bytes());
        chunk(&mut bytes, 2, 1, &clock);
        let mut th = Vec::new();
        for n in [1u32, 1, 0] {
            th.extend(n.to_le_bytes());
        }
        th.extend(1u64.to_le_bytes());
        th.extend(0u16.to_le_bytes());
        chunk(&mut bytes, 4, 2, &th);
        chunk(&mut bytes, 5, 3, &records(0, None));
        COMMITTED.with(|v| v.borrow_mut().clear());
        let good = crate::reader::read(&bytes).expect("valid complete prefix");
        assert_eq!(good.dependency_status().unwrap().records, 1);
        assert_eq!(COMMITTED.with(|v| v.borrow().clone()), vec![(3, 1)]);
        let base = bytes.clone();
        chunk(&mut bytes, 5, 4, &records(1, Some(255)));
        COMMITTED.with(|v| v.borrow_mut().clear());
        assert_eq!(
            crate::reader::read(&bytes).unwrap_err().rule,
            crate::model::Rule::UnknownRecordKind
        );
        assert_eq!(
            COMMITTED.with(|v| v.borrow().clone()),
            vec![(3, 1)],
            "the successful first record of chunk 4 must remain staged"
        );
        let mut foreign = base;
        foreign[12] = 9;
        let mut d = Vec::new();
        d.extend(1u32.to_le_bytes());
        d.extend(3u32.to_le_bytes());
        d.extend([1, 32, 1, 0]);
        d.extend(1u32.to_le_bytes());
        d.extend(2u32.to_le_bytes());
        d.extend([0; 32]);
        d.extend(0u16.to_le_bytes());
        chunk(&mut foreign, 8, 4, &d);
        let mut image = Vec::new();
        image.extend(1u32.to_le_bytes());
        image.extend(0u32.to_le_bytes());
        image.extend(1u16.to_le_bytes());
        image.push(b'x');
        image.extend(1u32.to_le_bytes());
        for n in [0u32, 256, 1] {
            image.extend(n.to_le_bytes());
        }
        image.extend(1u16.to_le_bytes());
        image.push(b'n');
        image.extend(0u16.to_le_bytes());
        chunk(&mut foreign, 11, 5, &image);
        let mut lane = Vec::new();
        for n in [1u32, 2, 3, 0, 1] {
            lane.extend(n.to_le_bytes());
        }
        lane.extend(77u64.to_le_bytes());
        lane.extend([0; 2]);
        chunk(&mut foreign, 12, 6, &lane);
        let mut ep = Vec::new();
        for n in [1u32, 3, 1] {
            ep.extend(n.to_le_bytes());
        }
        ep.extend([1, 0, 0, 0]);
        ep.extend([0; 16]);
        chunk(&mut foreign, 9, 7, &ep);
        fn events(first: u64, kinds: &[u8]) -> Vec<u8> {
            let mut p = Vec::new();
            p.extend(2u32.to_le_bytes());
            p.extend(1u32.to_le_bytes());
            p.extend(first.to_le_bytes());
            p.extend((kinds.len() as u32).to_le_bytes());
            for &kind in kinds {
                p.extend(900u32.to_le_bytes());
                p.extend([0, 0, kind, 0]);
            }
            p
        }
        chunk(&mut foreign, 13, 8, &events(0, &[3]));
        COMMITTED.with(|v| v.borrow_mut().clear());
        crate::reader::read(&foreign).expect("valid SPU prefix");
        assert_eq!(COMMITTED.with(|v| v.borrow().clone()), vec![(3, 1), (8, 1)]);
        chunk(&mut foreign, 13, 9, &events(1, &[1, 255]));
        COMMITTED.with(|v| v.borrow_mut().clear());
        assert!(crate::reader::read(&foreign).is_err());
        assert_eq!(
            COMMITTED.with(|v| v.borrow().clone()),
            vec![(3, 1), (8, 1)],
            "SPU semantic failure must not publish its valid first event"
        );
    }
}

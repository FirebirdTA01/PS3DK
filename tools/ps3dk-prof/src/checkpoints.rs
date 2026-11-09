//! Opt-in frame inventory evidence. A verified inventory is NOT completed work.
//! Source checkpoints (kind 21) are handled by the separate source proof parser.
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;
use std::collections::{BTreeMap, BTreeSet};

pub const FLAG_FRAME_CHECKPOINTS: u32 = 8;
pub const CHUNK_FRAME_MEMBERSHIP: u32 = 19;
pub const CHUNK_FRAME_CLOSE: u32 = 20;
pub const CHUNK_FRAME_SOURCE_CHECKPOINT: u32 = 21;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Source {
    pub kind: u8,
    pub id: u32,
    pub domain: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Frame {
    pub owner: u32,
    pub id: u64,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InventoryState {
    Verified,
    Unknown,
}
#[derive(Debug, Clone)]
pub struct Closure {
    pub chunk_seq: u64,
    pub frame: Frame,
    pub successor_seq: u64,
    pub members: Vec<Source>,
    pub state: InventoryState,
    pub membership_attempts: u64,
    pub membership_lost: u64,
}
#[derive(Debug, Clone, Default)]
pub struct Inventory {
    pub closed: BTreeMap<Frame, Closure>,
    pub uncertain: bool,
    /// Once refused, later metadata is not semantically validated or retained.
    /// The enclosing reader still checks every chunk's framing, CRC and sequence.
    pub refusal: Option<&'static str>,
    pub used_ops: u64,
    pub accounted_bytes: u64,
}
#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub records: u64,
    pub frames: u64,
    pub members_per_frame: u64,
    pub retained_members: u64,
    pub bytes: u64,
    pub ops: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self {
            records: 262144,
            frames: 65536,
            members_per_frame: 4096,
            retained_members: 1048576,
            bytes: 64 * 1024 * 1024,
            ops: 100_000_000,
        }
    }
}
#[derive(Debug)]
struct Binding {
    frame: Frame,
    source: Source,
    last: Option<u64>,
}
pub(crate) struct Reader {
    pub result: Inventory,
    budgets: Budgets,
    records: u64,
    retained_members: u64,
    ordinal: u64,
    seen: u64,
    last_binding: u64,
    bindings: BTreeMap<u64, Binding>,
    active: BTreeMap<Source, u64>,
    enrolled: BTreeSet<(Frame, Source)>,
    last_closed: BTreeMap<u32, u64>,
    frame_keys: BTreeSet<Frame>,
}
fn fail<T>(s: &str) -> Result<T, Malformed> {
    malformed(Rule::Checkpoint, s)
}
pub(crate) fn key(c: &mut Cursor<'_>) -> Result<Source, Malformed> {
    let kind = c.u8()?;
    let r0 = c.u8()?;
    let r1 = c.u16()?;
    let id = c.u32()?;
    let domain = c.u32()?;
    let r2 = c.u32()?;
    if r0 != 0
        || r1 != 0
        || r2 != 0
        || id == 0
        || !(1..=3).contains(&kind)
        || (kind == 1 && domain != 0)
        || (kind == 2 && domain == 0)
        || (kind == 3 && domain != id)
    {
        return fail("invalid checkpoint source key");
    }
    Ok(Source { kind, id, domain })
}
pub(crate) fn declared(source: Source, cap: &Capture, ext: Option<&crate::ext::ExtCapture>) -> bool {
    match source.kind {
        1 => cap.lanes.get(&source.id).is_some_and(|l| l.info.is_some()),
        2 => ext.is_some_and(|x| x.lanes.get(&source.id).is_some_and(|l| l.domain_id == source.domain)),
        3 => ext.is_some_and(|x| {
            x.domains
                .get(&source.id)
                .is_some_and(|d| d.kind == crate::ext::KIND_RSX_TIMESTAMP)
        }),
        _ => false,
    }
}
impl Reader {
    pub fn new(budgets: Budgets) -> Self {
        Self {
            result: Inventory::default(),
            budgets,
            records: 0,
            retained_members: 0,
            ordinal: 0,
            seen: 0,
            last_binding: 0,
            bindings: BTreeMap::new(),
            active: BTreeMap::new(),
            enrolled: BTreeSet::new(),
            last_closed: BTreeMap::new(),
            frame_keys: BTreeSet::new(),
        }
    }
    fn refuse(&mut self, reason: &'static str) -> bool {
        self.result.refusal = Some(reason);
        false
    }
    fn charge(&mut self, n: u64) -> bool {
        match self.result.used_ops.checked_add(n) {
            Some(v) if v <= self.budgets.ops => {
                self.result.used_ops = v;
                true
            }
            _ => self.refuse("checkpoint work budget"),
        }
    }
    /// Charge modeled visits/searches before parsing or allocation.
    /// Bytes are accounted allowances, not a claim about allocator RSS. Scratch
    /// covers the temporary expected roster as well as the retained output.
    fn admit(&mut self, payload: &[u8], members: u64, membership: bool, cap: &Capture) -> bool {
        if self.result.refusal.is_some() {
            return false;
        }
        if self.records >= self.budgets.records {
            return self.refuse("checkpoint record budget");
        }
        if members > self.budgets.members_per_frame {
            return self.refuse("checkpoint members per frame budget");
        }
        let scan = self.bindings.len() as u64
            + self.enrolled.len() as u64
            + self.active.len() as u64
            + self.result.closed.len() as u64
            + cap.lanes.len() as u64
            + self.last_closed.len() as u64
            + self.frame_keys.len() as u64
            + 1;
        // Include metadata visits, source lookup and ordered roster insertion.
        // These are policy work units, not measured CPU instructions.
        let work = 128 + payload.len() as u64 + 64 * scan + 64 * members;
        if !self.charge(work) {
            return false;
        }
        let retained = 1024 + 32 * members;
        let scratch = 128 * scan + 32 * members;
        if self
            .result
            .accounted_bytes
            .checked_add(retained)
            .and_then(|v| v.checked_add(scratch))
            .is_none_or(|v| v > self.budgets.bytes)
        {
            return self.refuse("checkpoint storage budget");
        }
        let new_members = members + u64::from(membership);
        if self
            .retained_members
            .checked_add(new_members)
            .is_none_or(|v| v > self.budgets.retained_members)
        {
            return self.refuse("checkpoint retained membership budget");
        }
        self.records += 1;
        self.retained_members += new_members;
        self.result.accounted_bytes += retained;
        true
    }
    fn frame_index(&mut self, f: Frame, cap: &Capture) -> Result<Option<usize>, Malformed> {
        let Some(lane) = cap.lanes.get(&f.owner).filter(|l| l.info.is_some()) else {
            return fail("checkpoint owner is not declared");
        };
        let n = lane.frames.len() as u64;
        if !self.charge(2 + u64::from(64 - n.leading_zeros())) {
            return Ok(None);
        }
        match lane.frames.binary_search_by_key(&f.id, |v| v.id) {
            Ok(i) => {
                if !self.frame_keys.contains(&f) {
                    if self.frame_keys.len() as u64 >= self.budgets.frames {
                        self.refuse("checkpoint frame budget");
                        return Ok(None);
                    }
                    self.frame_keys.insert(f);
                }
                Ok(Some(i))
            }
            Err(_) => fail("checkpoint frame is not previously recorded"),
        }
    }
    fn open_frame(&self, f: Frame) -> Result<(), Malformed> {
        if self.last_closed.get(&f.owner).is_some_and(|last| f.id <= *last) {
            return fail("membership changes an already closed frame");
        }
        Ok(())
    }
    /// Constant-time sizes for the caller's precharged membership scan.
    pub(crate) fn admission_scan_len(&self) -> u64 {
        self.bindings.len() as u64 + self.enrolled.len() as u64 + self.last_closed.len() as u64
    }
    pub(crate) fn participant_width(&self) -> u64 {
        1 + self.bindings.len() as u64 + self.enrolled.len() as u64
    }
    /// Caller charges frame-count times participant_width before visiting.
    /// This enumerates recorded membership, including idle open frames.
    pub(crate) fn visit_participants(&self, cap: &Capture, mut visit: impl FnMut(Frame, Source)) {
        for (&owner,lane) in &cap.lanes {
            for record in &lane.frames {
                let f = Frame { owner, id: record.id };
                visit(f,Source { kind:1,id:owner,domain:0 });
                for b in self.bindings.values() {
                    if b.frame.owner==owner && b.frame.id<=f.id && b.last.is_none_or(|last| last>=f.id) { visit(f,b.source); }
                }
                for &(frame,source) in &self.enrolled { if frame==f { visit(f,source); } }
            }
        }
    }
    pub(crate) fn admission_open(&self, f: Frame) -> Result<(), Malformed> {
        self.open_frame(f)
    }
    pub(crate) fn admission_member(&self, f: Frame, s: Source) -> Option<bool> {
        if self.result.refusal.is_some() || self.result.uncertain { return None; }
        Some((s.kind == 1 && s.id == f.owner)
            || self.enrolled.contains(&(f, s))
            || self.bindings.values().any(|b| b.source == s && b.frame.owner == f.owner
                && b.frame.id <= f.id && b.last.is_none_or(|last| last >= f.id)))
    }
    pub fn chunk(
        &mut self,
        ty: u32,
        payload: &[u8],
        chunk_seq: u64,
        cap: &Capture,
        ext: Option<&crate::ext::ExtCapture>,
    ) -> Result<(), Malformed> {
        if ty == CHUNK_FRAME_SOURCE_CHECKPOINT {
            return fail("source checkpoint dependency schema is not implemented");
        }
        if payload.len() < 48 {
            return fail("checkpoint payload shorter than 48 bytes");
        }
        let members = if ty == CHUNK_FRAME_CLOSE {
            u32::from_le_bytes(payload[40..44].try_into().unwrap()) as u64
        } else {
            0
        };
        if payload.len() as u64 != 48 + 16 * members {
            return fail("checkpoint payload length mismatch");
        }
        if !self.admit(payload, members, ty == CHUNK_FRAME_MEMBERSHIP, cap) {
            return Ok(());
        }
        let mut c = Cursor::new(payload, "checkpoint");
        if c.u16()? != 1 {
            return fail("unsupported checkpoint schema");
        }
        if ty == CHUNK_FRAME_MEMBERSHIP {
            let op = c.u8()?;
            if c.u8()? != 0 || !(1..=3).contains(&op) {
                return fail("invalid membership operation");
            }
            let f = Frame {
                owner: c.u32()?,
                id: c.u64()?,
            };
            let ordinal = c.u64()?;
            let source = key(&mut c)?;
            let binding = c.u64()?;
            if !declared(source, cap, ext) {
                return fail("membership source is not declared");
            }
            if self.frame_index(f, cap)?.is_none() {
                return Ok(());
            }
            self.open_frame(f)?;
            if ordinal <= self.ordinal || ordinal == u64::MAX {
                return fail("membership ordinal regressed or exhausted");
            }
            if ordinal != self.ordinal + 1 {
                self.result.uncertain = true;
            }
            self.ordinal = ordinal;
            self.seen += 1; // bounded by the record budget
            match op {
                1 => {
                    if binding <= self.last_binding || binding == u64::MAX {
                        return fail("binding id regressed or exhausted");
                    }
                    if self.active.contains_key(&source) && !self.result.uncertain {
                        return fail("source already bound");
                    }
                    self.last_binding = binding;
                    self.active.insert(source, binding);
                    self.bindings.insert(
                        binding,
                        Binding {
                            frame: f,
                            source,
                            last: None,
                        },
                    );
                }
                2 => {
                    if binding == 0 {
                        return fail("UNBIND needs a binding id");
                    }
                    if let Some(b) = self.bindings.get_mut(&binding) {
                        if b.frame.owner != f.owner || b.source != source || b.last.is_some() || f.id < b.frame.id {
                            return fail("UNBIND contradicts its binding");
                        }
                        b.last = Some(f.id);
                        if self.active.get(&source) == Some(&binding) {
                            self.active.remove(&source);
                        }
                    } else if !self.result.uncertain {
                        return fail("UNBIND has no BIND");
                    }
                    // A retained UNBIND witnesses an already allocated id even
                    // when its BIND was lost. Do not invent its interval, but
                    // keep the allocation frontier so later BIND cannot reuse it.
                    self.last_binding = self.last_binding.max(binding);
                }
                _ => {
                    if binding != 0 || (source.kind == 1 && source.id == f.owner) {
                        return fail("invalid ENROLL binding or implicit owner");
                    }
                    if !self.enrolled.insert((f, source)) {
                        return fail("duplicate ENROLL");
                    }
                }
            }
        } else {
            let flags = c.u16()?;
            let f = Frame {
                owner: c.u32()?,
                id: c.u64()?,
            };
            let successor_seq = c.u64()?;
            let attempts = c.u64()?;
            let lost = c.u64()?;
            c.u32()?;
            if c.u32()? != 0 || flags & !1 != 0 {
                return fail("invalid close flags or reserved field");
            }
            if self.result.closed.len() as u64 >= self.budgets.frames {
                self.refuse("checkpoint frame budget");
                return Ok(());
            }
            let Some(i) = self.frame_index(f, cap)? else {
                return Ok(());
            };
            self.open_frame(f)?;
            let lane = &cap.lanes[&f.owner];
            let Some(next) = lane.frames.get(i + 1) else {
                return fail("close lacks a recorded successor FRAME");
            };
            if next.seq != successor_seq {
                return fail("close successor sequence does not match");
            }
            if attempts < self.ordinal || attempts == u64::MAX || self.seen.checked_add(lost) != Some(attempts) {
                return fail("close membership frontier does not reconcile");
            }
            if self.result.uncertain || lost > 0 || attempts > self.ordinal {
                if flags & 1 == 0 {
                    return fail("close conceals membership uncertainty");
                }
                self.result.uncertain = true;
            }
            if flags & 1 != 0 {
                self.result.uncertain = true;
            }
            self.ordinal = attempts;
            let mut roster = Vec::with_capacity(members as usize);
            for _ in 0..members {
                let source = key(&mut c)?;
                if !declared(source, cap, ext) {
                    return fail("close source is not declared");
                }
                if roster.last().is_some_and(|last| *last >= source) {
                    return fail("close roster is not strictly sorted");
                }
                roster.push(source);
            }
            let mut expected = BTreeSet::new();
            expected.insert(Source {
                kind: 1,
                id: f.owner,
                domain: 0,
            });
            for b in self.bindings.values() {
                if b.frame.owner == f.owner && b.frame.id <= f.id && b.last.is_none_or(|last| last >= f.id) {
                    expected.insert(b.source);
                }
            }
            for &(at, source) in &self.enrolled {
                if at == f {
                    expected.insert(source);
                }
            }
            if !self.result.uncertain && !expected.iter().copied().eq(roster.iter().copied()) {
                return fail("close roster differs from evidenced membership");
            }
            // Existing Lane retains a total GAP count, not interval-local ranges.
            // Be conservative in this slice; do not infer an unambiguous boundary.
            let state = if self.result.uncertain || lane.gap_count != 0 || f.id.checked_add(1) != Some(next.id) {
                InventoryState::Unknown
            } else {
                InventoryState::Verified
            };
            self.last_closed.insert(f.owner, f.id);
            self.result.closed.insert(
                f,
                Closure {
                    chunk_seq,
                    frame: f,
                    successor_seq,
                    members: roster,
                    state,
                    membership_attempts: attempts,
                    membership_lost: lost,
                },
            );
        }
        c.done()
    }
}

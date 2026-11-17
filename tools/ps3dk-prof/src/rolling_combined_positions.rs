//! Pure global-position feasibility for a future combined rolling checkpoint.
//!
//! This checks an abstract allocation of omitted event chunks, not their actual
//! historical interleaving or contents. The caller validates original framing,
//! family identities, declaration chronology, event/byte counters and typed
//! cursor state. No file reader or runtime rolling capability is enabled here.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

/// One PPU incarnation's RECORDS chunk demand in a legal lifetime interval.
/// `start` is inclusive and `end` exclusive. Zero demand is permitted, but its
/// interval is still validated. The caller derives this from THREADS/RETIRED.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PpuDemand {
    pub start: u64,
    pub end: u64,
    pub chunks: u64,
}

/// One support-free SPU block. Endpoints are inclusive, original chunk indices
/// and must themselves be SPU_EVENTS. Blocks must be ordered and disjoint.
/// No per-lane position or order within a block is claimed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpuBlock {
    pub first: u64,
    pub last: u64,
    pub chunks: u64,
}

#[derive(Debug, Clone, Copy)]
pub struct Limits {
    /// Structural bound on temporary entries: metadata + twice the SPU block
    /// count for anchors, plus two queues of (PPU demand + SPU block) entries.
    /// Checked before allocation; entries have fixed size, independent of slots.
    pub max_temporary_items: usize,
    /// Structural proof work: input/merge/sweep steps, rank comparisons, and
    /// conservative heap-operation units. Independent of chunk multiplicities.
    /// A caller checking several checkpoints must also enforce an outer budget.
    pub max_work: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { max_temporary_items: 65_536, max_work: 4_000_000 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Invalid(&'static str),
    Infeasible,
    Limit(&'static str),
}

struct Work { remaining: u64 }
impl Work {
    fn charge(&mut self, amount: u64) -> Result<(), Error> {
        self.remaining = self.remaining.checked_sub(amount).ok_or(Error::Limit("position work"))?;
        Ok(())
    }
    fn heap(&mut self, size: usize) -> Result<(), Error> {
        // Push/pop each traverse at most a heap height, with at most two
        // ordering comparisons per level. Charge before the library operation.
        let height = usize::BITS - size.leading_zeros();
        self.charge(1 + 2 * u64::from(height))
    }
}

type Queue = BinaryHeap<Reverse<(u64, u64, u64)>>;

fn push(queue: &mut Queue, item: (u64, u64, u64), work: &mut Work) -> Result<(), Error> {
    let size = queue.len().checked_add(1).ok_or(Error::Limit("position queue"))?;
    // All queue storage is reserved up front. Never grow with multiplicity.
    if size > queue.capacity() { return Err(Error::Limit("position queue capacity")); }
    work.heap(size)?;
    queue.push(Reverse(item));
    Ok(())
}

fn pop(queue: &mut Queue, work: &mut Work) -> Result<Option<(u64, u64, u64)>, Error> {
    work.heap(queue.len())?;
    Ok(queue.pop().map(|Reverse(item)| item))
}

fn rank(index: u64, excluded: &[u64], work: &mut Work) -> Result<u64, Error> {
    let (mut low, mut high) = (0, excluded.len());
    while low < high {
        work.charge(1)?;
        let middle = low + (high - low) / 2;
        if excluded[middle] < index { low = middle + 1; } else { high = middle; }
    }
    let before = u64::try_from(low).map_err(|_| Error::Invalid("anchor count overflow"))?;
    index.checked_sub(before).ok_or(Error::Invalid("free rank underflow"))
}

fn demand(queue: &mut Queue, start: u64, end: u64, count: u64, excluded: &[u64],
    total: &mut u64, work: &mut Work) -> Result<(), Error> {
    if count == 0 { return Ok(()); }
    let release = rank(start, excluded, work)?;
    let deadline = rank(end, excluded, work)?;
    if count > deadline.checked_sub(release).ok_or(Error::Invalid("demand interval"))? {
        return Err(Error::Infeasible);
    }
    *total = total.checked_add(count).ok_or(Error::Invalid("free demand overflow"))?;
    push(queue, (release, deadline, count), work)
}

/// Prove full, disjoint occupancy of `[0, frontier)` by original metadata,
/// PPU RECORDS and SPU_EVENTS. Metadata indices must be strictly increasing;
/// SPU block endpoints cannot coincide with metadata. `frontier == u64::MAX`
/// is permitted: the largest legal original index is then `u64::MAX - 1`.
///
/// Success is an existential position proof, never reconstructed event order.
/// This surface does not check whether SPU support occurs inside a block; the
/// caller must validate its typed journal and metadata family chronology.
pub fn verify_positions(
    frontier: u64,
    metadata: &[u64],
    ppu: &[PpuDemand],
    spu: &[SpuBlock],
    limits: Limits,
) -> Result<(), Error> {
    let anchors = spu.len().checked_mul(2).and_then(|n| n.checked_add(metadata.len()))
        .ok_or(Error::Limit("position anchors"))?;
    let demands = ppu.len().checked_add(spu.len()).ok_or(Error::Limit("position demands"))?;
    let temporary = demands.checked_mul(2).and_then(|n| n.checked_add(anchors))
        .ok_or(Error::Limit("position temporary entries"))?;
    if temporary > limits.max_temporary_items { return Err(Error::Limit("position temporary entries")); }
    let mut work = Work { remaining: limits.max_work };
    work.charge(1)?;

    // Validate every original anchor/range and exact global chunk total before
    // allocating. A zero demand does not exempt malformed interval bounds.
    let mut previous = None;
    let mut total = u64::try_from(metadata.len()).map_err(|_| Error::Invalid("metadata count overflow"))?;
    for &index in metadata {
        work.charge(1)?;
        if index >= frontier || previous.is_some_and(|p| p >= index) {
            return Err(Error::Invalid("metadata index/order"));
        }
        previous = Some(index);
    }
    for d in ppu {
        work.charge(1)?;
        if d.start > d.end || d.end > frontier { return Err(Error::Invalid("PPU interval")); }
        total = total.checked_add(d.chunks).ok_or(Error::Invalid("chunk count overflow"))?;
    }
    previous = None;
    for b in spu {
        work.charge(1)?;
        if b.first > b.last || b.last >= frontier || previous.is_some_and(|end| end >= b.first) {
            return Err(Error::Invalid("SPU block range/order"));
        }
        let width = b.last.checked_sub(b.first).and_then(|n| n.checked_add(1))
            .ok_or(Error::Invalid("SPU block width overflow"))?;
        if b.chunks == 0 || b.chunks > width || (b.chunks == 1) != (b.first == b.last) {
            return Err(Error::Invalid("SPU block count/endpoints"));
        }
        total = total.checked_add(b.chunks).ok_or(Error::Invalid("chunk count overflow"))?;
        previous = Some(b.last);
    }

    // Merge already-ordered metadata and forced SPU endpoints. No all-history
    // bitmap, slot array, or per-chunk label is constructed.
    let mut excluded = Vec::new();
    excluded.try_reserve_exact(anchors).map_err(|_| Error::Limit("position anchor allocation"))?;
    let mut metadata_at = 0;
    for b in spu {
        for (which, endpoint) in [b.first, b.last].into_iter().enumerate() {
            if which == 1 && b.first == b.last { continue; }
            work.charge(1)?;
            while metadata_at < metadata.len() && metadata[metadata_at] < endpoint {
                work.charge(1)?;
                excluded.push(metadata[metadata_at]);
                metadata_at += 1;
            }
            if metadata.get(metadata_at) == Some(&endpoint) {
                return Err(Error::Invalid("metadata occupies forced SPU endpoint"));
            }
            excluded.push(endpoint);
        }
    }
    for &index in &metadata[metadata_at..] {
        work.charge(1)?;
        excluded.push(index);
    }
    if total != frontier { return Err(Error::Infeasible); }
    let excluded_count = u64::try_from(excluded.len()).map_err(|_| Error::Invalid("anchor count overflow"))?;
    let available = frontier.checked_sub(excluded_count).ok_or(Error::Invalid("anchor count exceeds frontier"))?;

    // Pending is ordered by release; ready is ordered by deadline. Reserve
    // both heaps for at most one entry per input demand, never per chunk.
    let mut pending = Queue::new();
    let mut ready = Queue::new();
    pending.try_reserve_exact(demands).map_err(|_| Error::Limit("position pending allocation"))?;
    ready.try_reserve_exact(demands).map_err(|_| Error::Limit("position ready allocation"))?;
    let mut free_demand = 0u64;
    for d in ppu {
        work.charge(1)?;
        demand(&mut pending, d.start, d.end, d.chunks, &excluded, &mut free_demand, &mut work)?;
    }
    for b in spu {
        work.charge(1)?;
        let forced = if b.first == b.last { 1 } else { 2 };
        let count = b.chunks.checked_sub(forced).ok_or(Error::Invalid("SPU forced count"))?;
        let end = b.last.checked_add(1).ok_or(Error::Invalid("SPU end overflow"))?;
        demand(&mut pending, b.first, end, count, &excluded, &mut free_demand, &mut work)?;
    }
    if free_demand != available { return Err(Error::Infeasible); }

    // EDF is complete for interval-constrained unit slots. Each batch either
    // exhausts a demand, reaches a new release, or proves a missed deadline.
    // A huge multiplicity therefore does not add iterations.
    let mut cursor = 0u64;
    while cursor < available {
        work.charge(1)?;
        while pending.peek().is_some_and(|Reverse((release, _, _))| *release <= cursor) {
            let Some((release, deadline, count)) = pop(&mut pending, &mut work)? else {
                return Err(Error::Invalid("pending queue invariant"));
            };
            if deadline <= cursor { return Err(Error::Infeasible); }
            push(&mut ready, (deadline, release, count), &mut work)?;
        }
        let Some((deadline, release, count)) = pop(&mut ready, &mut work)? else {
            // Jumping to the next release would leave original slots unowned.
            return Err(Error::Infeasible);
        };
        if deadline <= cursor || release > cursor { return Err(Error::Infeasible); }
        let next_release = pending.peek().map_or(available, |Reverse((release, _, _))| *release);
        let boundary = deadline.min(next_release).min(available);
        let capacity = boundary.checked_sub(cursor).ok_or(Error::Invalid("sweep boundary"))?;
        let assigned = count.min(capacity);
        if assigned == 0 { return Err(Error::Infeasible); }
        cursor = cursor.checked_add(assigned).ok_or(Error::Invalid("sweep overflow"))?;
        if count > assigned {
            if cursor == deadline { return Err(Error::Infeasible); }
            push(&mut ready, (deadline, release, count - assigned), &mut work)?;
        }
    }
    if !pending.is_empty() || !ready.is_empty() { return Err(Error::Infeasible); }
    Ok(())
}

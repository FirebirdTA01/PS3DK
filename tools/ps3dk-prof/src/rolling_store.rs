//! Physical PS3DKROL v1/v2 reader. Checkpoints remain opaque: a successful read
//! proves container/chunk integrity, not semantic restart validity.
//!
//! The immutable input must be a coherent snapshot taken by the caller. This
//! module supplies neither a live-file retry protocol nor runtime integration.
//! It selects one complete committed root chain; it never scans unused slots.

const PAGE: usize = 4096;
const PREFIX: usize = 12288;
const HEAD: usize = 96;
const FOOT: usize = 64;
const ENTRY: usize = 40;
const MAX_CHUNK: usize = 1_048_576;

/// Caller admission limits. No file-sized scratch allocation is performed.
#[derive(Clone, Copy, Debug)]
pub struct Limits { pub max_file_bytes: u64, pub max_slot_bytes: u32 }
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error { Budget(&'static str), InvalidConfig(&'static str), NoCommittedRoot, InconsistentRoots }
/// Configuration/session identity read from the validated configuration page.
/// The container cap and the caller's admission limits are separate constraints.
#[derive(Clone, Debug)]
pub struct Config {
    pub format_version: u32,
    pub file_bytes: u64, pub max_bytes: u64, pub retained_slots: u32,
    pub checkpoint_bytes: u32, pub payload_bytes: u32, pub slot_bytes: u32, pub session: [u8; 16],
}
#[derive(Clone, Debug)]
pub struct View {
    /// V1 state is 0 (unspecified). V2: 1 OPEN, 2 SEALED, 3 STOPPED_RESOURCE.
    /// SEALED is a physical declaration, not proof of semantic finals or close.
    pub format_version: u32, pub state: u32, pub reason: u32, pub bound_payload_bytes: u64,
    pub generation: u64, pub first_segment: u64, pub next_segment: u64,
    pub first_chunk: u64, pub next_chunk: u64, pub evicted_segments: u64,
    pub evicted_chunks: u64, pub evicted_payload_bytes: u64,
    pub retained_segments: u32,
    /// A root chain was invalid, except for the normal zero alternate root of
    /// a newly formatted, empty generation-1 container. Matches C pfr_open.
    pub recovered: bool,
}
/// An oldest-first committed segment. Both slices borrow the supplied input;
/// chunk sequences are original sequences, never renumbered after eviction.
#[derive(Clone, Debug)]
pub struct Segment<'a> {
    pub physical_slot: u32, pub segment: u64, pub first_chunk: u64, pub next_chunk: u64,
    pub checkpoint: &'a [u8], pub payload: &'a [u8],
}
#[derive(Debug)]
pub struct Committed<'a> { pub config: Config, pub view: View, pub segments: Vec<Segment<'a>> }

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Budget(reason) => write!(f, "rolling store admission limit: {reason}"),
            Self::InvalidConfig(reason) => write!(f, "invalid rolling store configuration: {reason}"),
            Self::NoCommittedRoot => f.write_str("no complete valid rolling store root chain"),
            Self::InconsistentRoots => f.write_str("contradictory complete rolling store roots"),
        }
    }
}
impl std::error::Error for Error {}

// Callers establish all containing page/chunk bounds before these fixed reads.
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn u64_at(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) }
fn zeros(b: &[u8]) -> bool { b.iter().all(|&n| n == 0) }
fn crc32(b: &[u8]) -> u32 {
    let mut c = u32::MAX;
    for &v in b {
        c ^= u32::from(v);
        for _ in 0..8 { c = (c >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(c & 1)); }
    }
    !c
}
fn crc_matches(b: &[u8], at: usize) -> bool { u32_at(b, at) == crc32(&b[..at]) }

fn configuration(bytes: &[u8], limits: Limits) -> Result<Config, Error> {
    // Admission of the actual byte slice precedes even the first field read.
    if bytes.len() as u64 > limits.max_file_bytes { return Err(Error::Budget("file bytes")); }
    if bytes.len() < PREFIX { return Err(Error::InvalidConfig("short container")); }
    let b = &bytes[..PAGE];
    let file_bytes = u64_at(b, 16);
    let slot_bytes = u32_at(b, 44);
    if file_bytes > limits.max_file_bytes { return Err(Error::Budget("declared file bytes")); }
    if slot_bytes > limits.max_slot_bytes { return Err(Error::Budget("slot bytes")); }
    let format_version = u32_at(b, 8);
    if &b[..8] != b"PS3DKROL" || !matches!(format_version, 1 | 2) || u32_at(b, 12) != PAGE as u32 {
        return Err(Error::InvalidConfig("magic/version/page"));
    }
    if !zeros(&b[64..PAGE - 4]) || !crc_matches(b, PAGE - 4) {
        return Err(Error::InvalidConfig("reserved bytes/CRC"));
    }
    let config = Config {
        format_version,
        file_bytes, slot_bytes, max_bytes: u64_at(b, 24), retained_slots: u32_at(b, 32),
        checkpoint_bytes: u32_at(b, 36), payload_bytes: u32_at(b, 40),
        session: b[48..64].try_into().unwrap(),
    };
    if !(2..=64).contains(&config.retained_slots) || config.payload_bytes < 20 {
        return Err(Error::InvalidConfig("slot count/payload capacity"));
    }
    // Two u32 capacities plus fixed overhead fit u64. At most 65 slots, each
    // bounded to u32, also fit u64; never allocate from these declared sizes.
    let wanted_slot = (HEAD as u64 + u64::from(config.checkpoint_bytes)
        + u64::from(config.payload_bytes) + FOOT as u64 + PAGE as u64 - 1)
        & !(PAGE as u64 - 1);
    if wanted_slot > u64::from(u32::MAX) || wanted_slot > usize::MAX as u64
        || wanted_slot != u64::from(slot_bytes) {
        return Err(Error::InvalidConfig("slot arithmetic"));
    }
    let wanted_file = PREFIX as u64 + (u64::from(config.retained_slots) + 1) * wanted_slot;
    if file_bytes != wanted_file || bytes.len() as u64 != file_bytes || file_bytes > config.max_bytes {
        return Err(Error::InvalidConfig("file length/cap"));
    }
    Ok(config)
}

fn chunks(payload: &[u8], first: u64, next: u64) -> bool {
    let mut remaining = payload;
    let mut seq = first;
    while !remaining.is_empty() {
        if remaining.len() < 20 { return false; }
        let len = u32_at(remaining, 4) as usize;
        if len > MAX_CHUNK { return false; }
        let total = len + 20; // bounded by MAX_CHUNK
        if total > remaining.len() || seq == u64::MAX || u64_at(remaining, 8) != seq
            || !crc_matches(&remaining[..total], total - 4) { return false; }
        seq += 1;
        remaining = &remaining[total..];
    }
    seq == next
}

#[derive(Clone, Copy)]
struct SlotEntry { physical: u32, checkpoint: u32, payload: u32, segment: u64, first: u64, next: u64 }

fn slot<'a>(bytes: &'a [u8], cfg: &Config, e: SlotEntry) -> Option<Segment<'a>> {
    let slot_bytes = cfg.slot_bytes as usize;
    let at = PREFIX.checked_add((e.physical as usize).checked_mul(slot_bytes)?)?;
    let b = bytes.get(at..at.checked_add(slot_bytes)?)?;
    let footer_at = slot_bytes.checked_sub(FOOT)?;
    let f = &b[footer_at..];
    let cp_end = HEAD.checked_add(e.checkpoint as usize)?;
    let used = cp_end.checked_add(e.payload as usize)?;
    if used > footer_at { return None; }
    let checkpoint = &b[HEAD..cp_end];
    let payload = &b[cp_end..used];
    let generation = e.segment.checked_add(2)?;
    let cc = crc32(checkpoint);
    let pc = crc32(payload);
    if &b[..8] != b"PFRSLOT1" || b[8..24] != cfg.session
        || u64_at(b, 24) != e.segment || u64_at(b, 32) != generation
        || u64_at(b, 40) != e.first || u64_at(b, 48) != e.next
        || u32_at(b, 56) != e.checkpoint || u32_at(b, 60) != e.payload
        || u32_at(b, 64) != cc || u32_at(b, 68) != pc || !zeros(&b[72..80])
        || !crc_matches(b, 80) || !zeros(&b[84..HEAD]) || !zeros(&b[used..footer_at])
        || &f[..8] != b"PFRCMIT1" || u64_at(f, 8) != e.segment || u64_at(f, 16) != generation
        || u64_at(f, 24) != e.first || u64_at(f, 32) != e.next
        || u32_at(f, 40) != cc || u32_at(f, 44) != pc || u32_at(f, 48) != crc32(&b[..footer_at])
        || !zeros(&f[52..60]) || !crc_matches(f, 60) || !chunks(payload, e.first, e.next) {
        return None;
    }
    Some(Segment { physical_slot: e.physical, segment: e.segment, first_chunk: e.first,
        next_chunk: e.next, checkpoint, payload })
}

fn root<'a>(bytes: &'a [u8], cfg: &Config, which: usize) -> Option<(View, Vec<Segment<'a>>)> {
    let b = &bytes[(which + 1) * PAGE..(which + 2) * PAGE];
    let v2 = cfg.format_version == 2;
    let head = if v2 { 128 } else { HEAD };
    if &b[..8] != (if v2 { b"PFRROOT2" } else { b"PFRROOT1" }) || !crc_matches(b, PAGE - 4) || u32_at(b, 20) != 0
        || b[80..96] != cfg.session { return None; }
    let view = View {
        format_version: cfg.format_version, state: if v2 { u32_at(b, 96) } else { 0 },
        reason: if v2 { u32_at(b, 100) } else { 0 }, bound_payload_bytes: if v2 { u64_at(b, 112) } else { 0 },
        generation: u64_at(b, 8), retained_segments: u32_at(b, 16),
        first_segment: u64_at(b, 24), next_segment: u64_at(b, 32),
        first_chunk: u64_at(b, 40), next_chunk: u64_at(b, 48),
        evicted_segments: u64_at(b, 56), evicted_chunks: u64_at(b, 64),
        evicted_payload_bytes: u64_at(b, 72), recovered: false,
    };
    if v2 && (!(1..=3).contains(&view.state) || u64_at(b, 104) != view.next_chunk || u64_at(b, 120) != 0
        || (view.state != 3 && view.reason != 0) || (view.state == 3 && !(1..=5).contains(&view.reason))) { return None; }
    let minimum_revision = view.next_segment.checked_add(1)?;
    if view.generation == 0 || view.generation & 1 != which as u64
        || (if v2 { view.generation < minimum_revision } else { view.generation != minimum_revision })
        || view.retained_segments > cfg.retained_slots
        || view.next_segment.checked_sub(view.first_segment) != Some(u64::from(view.retained_segments))
        || view.first_segment != view.evicted_segments || view.first_chunk != view.evicted_chunks
        || view.first_chunk > view.next_chunk { return None; }
    // C permits an unbounded mathematical ceiling when this product exceeds
    // u64, since every representable evicted byte count then lies below it.
    if let Some(ceiling) = view.evicted_segments.checked_mul(u64::from(cfg.payload_bytes)) {
        if view.evicted_payload_bytes > ceiling { return None; }
    }
    if view.retained_segments == 0 && ((!v2 && view.generation != 1) || view.first_chunk != 0
        || view.next_chunk != 0 || view.evicted_payload_bytes != 0) { return None; }
    if u64::from(view.retained_segments) != view.next_segment.min(u64::from(cfg.retained_slots)) {
        return None;
    }
    let count = view.retained_segments as usize; // already bounded to <=64
    if !zeros(&b[head + count * ENTRY..PAGE - 4]) { return None; }
    let mut seen = [false; 65];
    let mut segments = Vec::with_capacity(count);
    let mut cursor = view.first_chunk;
    let mut bound = view.evicted_payload_bytes;
    for i in 0..count {
        let at = head + i * ENTRY;
        let e = SlotEntry { physical: u32_at(b, at), checkpoint: u32_at(b, at + 4),
            payload: u32_at(b, at + 8), segment: u64_at(b, at + 16),
            first: u64_at(b, at + 24), next: u64_at(b, at + 32) };
        if e.physical > cfg.retained_slots || e.checkpoint > cfg.checkpoint_bytes
            || e.payload > cfg.payload_bytes || u32_at(b, at + 12) != 0
            || e.segment != view.first_segment + i as u64 || e.first != cursor
            || e.next < e.first || e.next - e.first > u64::from(e.payload / 20)
            || seen[e.physical as usize] { return None; }
        seen[e.physical as usize] = true;
        segments.push(slot(bytes, cfg, e)?);
        if v2 { bound = bound.checked_add(u64::from(e.payload))?; }
        cursor = e.next;
    }
    if cursor != view.next_chunk || (v2 && bound != view.bound_payload_bytes) { return None; }
    Some((view, segments))
}

fn same_segment(a: &Segment<'_>, b: &Segment<'_>) -> bool {
    // Both borrow one validated input snapshot: identical slot identity and
    // ranges/lengths already imply identical checkpoint/payload bytes.
    a.physical_slot == b.physical_slot && a.segment == b.segment && a.first_chunk == b.first_chunk
        && a.next_chunk == b.next_chunk && a.checkpoint.len() == b.checkpoint.len() && a.payload.len() == b.payload.len()
}
fn successor(cfg: &Config, a: &(View, Vec<Segment<'_>>), b: &(View, Vec<Segment<'_>>)) -> bool {
    let (x, before) = a; let (y, after) = b;
    if x.generation.checked_add(1) != Some(y.generation) || x.state != 1 { return false; }
    if x.next_segment == y.next_segment {
        return x.first_segment == y.first_segment && x.first_chunk == y.first_chunk && x.next_chunk == y.next_chunk
            && x.evicted_segments == y.evicted_segments && x.evicted_chunks == y.evicted_chunks
            && x.evicted_payload_bytes == y.evicted_payload_bytes && x.bound_payload_bytes == y.bound_payload_bytes
            && before.len() == after.len() && before.iter().zip(after).all(|(a,b)| same_segment(a,b));
    }
    if x.next_segment.checked_add(1) != Some(y.next_segment) || y.state != 1 { return false; }
    let dropped = usize::from(before.len() == cfg.retained_slots as usize);
    let survivors = &before[dropped..];
    if after.len() != survivors.len() + 1 || !survivors.iter().zip(after).all(|(a,b)| same_segment(a,b)) { return false; }
    let last = after.last().unwrap(); // at least one appended segment
    if last.segment != x.next_segment || last.first_chunk != x.next_chunk
        || x.bound_payload_bytes.checked_add(last.payload.len() as u64) != Some(y.bound_payload_bytes)
        || before.iter().any(|s| s.physical_slot == last.physical_slot) { return false; }
    if dropped != 0 {
        x.evicted_segments.checked_add(1) == Some(y.evicted_segments) && y.evicted_chunks == before[0].next_chunk
            && x.evicted_payload_bytes.checked_add(before[0].payload.len() as u64) == Some(y.evicted_payload_bytes)
    } else {
        x.evicted_segments == y.evicted_segments && x.evicted_chunks == y.evicted_chunks
            && x.evicted_payload_bytes == y.evicted_payload_bytes
    }
}

/// Validate both root chains and select the newest complete one. At most two
/// sets of 64 descriptors are allocated; checkpoint/payload bytes stay borrowed.
/// A damaged alternate chain is reported even when the newest chain is valid.
pub fn read(bytes: &[u8], limits: Limits) -> Result<Committed<'_>, Error> {
    let config = configuration(bytes, limits)?;
    let mut even = root(bytes, &config, 0);
    let mut odd = root(bytes, &config, 1);
    let missing_even = zeros(&bytes[PAGE..2 * PAGE]);
    let missing_odd = zeros(&bytes[2 * PAGE..PREFIX]);
    let damaged = (even.is_none() && !missing_even) || (odd.is_none() && !missing_odd);
    if config.format_version == 2 {
        if let (Some(e), Some(o)) = (&even, &odd) {
            let valid = if e.0.generation < o.0.generation { successor(&config, e, o) } else { successor(&config, o, e) };
            if !valid { return Err(Error::InconsistentRoots); }
        }
    }
    let (mut view, segments) = match (&even, &odd) {
        (None, None) => return Err(Error::NoCommittedRoot),
        (Some(_), None) => even.take().unwrap(),
        (None, Some(_)) => odd.take().unwrap(),
        (Some(e), Some(o)) => if o.0.generation > e.0.generation { odd.take().unwrap() } else { even.take().unwrap() },
    };
    view.recovered = damaged || ((missing_even || missing_odd) && view.generation != 1);
    Ok(Committed { config, view, segments })
}

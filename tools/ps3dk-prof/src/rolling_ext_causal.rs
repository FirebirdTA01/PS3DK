//! Retained endpoint placement from causal support only.
//! Raw-counter, tick-lift and RMAX1 history are explicitly not qualified.
//! The caller supplies classification for this original capture, and owns
//! outer framing, PPU/PROFILE parsing and historical cross-family references.
use std::collections::{BTreeMap, BTreeSet};
use crate::profile::Classification;
use crate::ext::rolling::Retained;
use crate::ext::{Bracket, Drain, LaneOrder, OpenBegins, SourceKey, Structure};

#[derive(Clone, Copy, Debug)]
pub struct CausalLimits {
    pub work_items: u64,
    pub groups: usize,
    pub endpoint_rows: usize,
    /// Display retention only; this limit never disables contradiction checks.
    pub finding_rows: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlacementBasis { CausalSupportOnly }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CounterHistory { NotEvaluated }
pub const CAUSAL_ONLY_NOTE: &str = "Causal support only; raw-counter, tick-lift and RMAX1 history have not been evaluated.";
pub struct CausalOptions<'a> {
    /// Must classify this original capture's PROFILE. None means unreviewed.
    pub classification: Option<&'a Classification>,
    pub limits: CausalLimits,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Bounds {
    pub lower: Option<u64>,
    pub upper_drain: Option<u64>,
    pub upper_bracket: Option<u64>,
    pub upper: Option<u64>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Refusal {
    NoOrder,
    NoOrderFinal,
    SourceIncomplete,
    InvalidSource,
    InvalidSupport,
    SourceLoss { prefix: u64, retained: u64 },
    ProtocolError,
    OpenCountUnverified,
    OrderDropped(u64),
    UnpositionedBrackets { epoch: u32, count: u64 },
    DrainTbDecrease,
    CausalOrderContradiction { epoch: u32 },
    ObservationEpochOrder,
    MissingLower,
    MissingUpper,
    EmptyInterval,
    Resource(&'static str),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Placement {
    /// Qualified causal support only, not ticks/RMAX1/duration.
    QualifiedCausal { lower: u64, upper: u64 },
    /// Profile failures are retained once in CausalView.classification.
    ConditionalCausal { lower: u64, upper: u64 },
    Unavailable(Vec<Refusal>),
}
pub struct Endpoint {
    pub seq: u64,
    pub epoch: u32,
    pub bounds: Bounds,
    pub placement: Placement,
}
pub enum ObservationId {
    Event(u64),
    Bracket { epoch: u32, sequence: u64 },
}
pub struct ContradictionWitness {
    pub previous: ObservationId,
    pub next: ObservationId,
    pub lower: u64,
    pub upper: u64,
}
pub struct OrderSummary {
    pub final_present: bool,
    pub dropped: Option<u64>,
    pub first_tb_decrease: Option<(u64, u64, u64)>,
    pub unpositioned: BTreeMap<u32, u64>,
    pub contradicted_epochs: BTreeSet<u32>,
    pub observation_epochs_decrease: bool,
    pub witnesses: Vec<ContradictionWitness>,
}
pub struct Lane {
    pub domain_id: u32,
    pub order: OrderSummary,
    /// Actual retained source events only, strictly increasing source sequence.
    pub endpoints: Vec<Endpoint>,
}
pub struct CausalView {
    pub basis: PlacementBasis,
    pub counter_history: CounterHistory,
    pub note: &'static str,
    pub classification: Option<Classification>,
    /// Empty on resource refusal; no partially qualified lane rows escape.
    pub lanes: BTreeMap<u32, Lane>,
    pub refusal: Option<Refusal>,
    pub work_items: u64,
}
pub struct Finished {
    pub retained: Retained,
    pub causal: CausalView,
}

impl Default for CausalLimits {
    fn default() -> Self {
        Self { work_items: 1 << 24, groups: 262144, endpoint_rows: 65536, finding_rows: 32 }
    }
}

impl super::Adapter {
    /// Causal support only. Historical raw counters, modular tick lifting and
    /// RMAX1 are not evaluated. Structural errors take precedence over a causal
    /// budget refusal, which atomically publishes no endpoint rows.
    pub fn finish_with_causal(self, has_trailer: bool, trailer_ok: bool,
        frames: &BTreeMap<u32, BTreeSet<u64>>, options: CausalOptions<'_>)
        -> Result<Finished, super::Error> {
        self.healthy()?;
        let mut budget = Budget::new(options.limits);
        let prepared = seed(&self, &mut budget);
        // Always finish the actual structural parser, including on a causal
        // allocation/work refusal. No budget failure waives its validators.
        let retained = self.finish(has_trailer, trailer_ok, frames)?;
        let result = prepared.and_then(|seeds| evaluate(&retained, seeds, options.classification, &mut budget));
        let (lanes, classification, refusal) = match result {
            Ok((lanes, classification)) => (lanes, classification, None),
            Err(why) => (BTreeMap::new(), None, Some(why)),
        };
        Ok(Finished { retained, causal: CausalView { basis: PlacementBasis::CausalSupportOnly,
            counter_history: CounterHistory::NotEvaluated, note: CAUSAL_ONLY_NOTE,
            classification, lanes, refusal, work_items: budget.used } })
    }
}

type Checked<T> = Result<T, Refusal>;
fn map_cost(n: usize) -> u64 { 2 + u64::from(usize::BITS - n.leading_zeros()) }

struct Budget { limits: CausalLimits, used: u64, groups: usize, findings: usize }
impl Budget {
    fn new(limits: CausalLimits) -> Self { Self { limits, used: 0, groups: 0, findings: 0 } }
    fn charge(&mut self, n: u64) -> Checked<()> {
        let next = self.used.checked_add(n).ok_or(Refusal::Resource("work"))?;
        if next > self.limits.work_items { return Err(Refusal::Resource("work")); }
        self.used = next; Ok(())
    }
    fn reserve<T>(&mut self, v: &mut Vec<T>, n: usize) -> Checked<()> {
        self.charge(n as u64 + 1)?;
        v.try_reserve(n).map_err(|_| Refusal::Resource("allocation"))
    }
    fn group(&mut self, groups: &mut Vec<Group>, group: Group) -> Checked<()> {
        if self.groups >= self.limits.groups { return Err(Refusal::Resource("groups")); }
        self.charge(1)?; self.reserve(groups, 1)?; groups.push(group); self.groups += 1; Ok(())
    }
}

#[derive(Clone, Copy)]
struct EventRange { first: u64, end: u64, epoch: u32 }
struct Seed { domain: u32, ranges: Vec<EventRange> }
fn seed(adapter: &super::Adapter, budget: &mut Budget) -> Checked<BTreeMap<u32, Seed>> {
    let mut seeds = BTreeMap::new();
    for (&lane, info) in &adapter.reader.cap.lanes {
        budget.charge(map_cost(seeds.len()))?;
        seeds.insert(lane, Seed { domain: info.domain_id, ranges: Vec::new() });
    }
    for (&lane, &domain) in &adapter.reader.forward {
        budget.charge(map_cost(seeds.len()))?;
        seeds.insert(lane, Seed { domain, ranges: Vec::new() });
    }
    let mut count = 0usize;
    for entry in &adapter.journal {
        budget.charge(1)?;
        if let super::Entry::Events(block) = entry {
            for run in block.runs.values() {
                if count >= budget.limits.groups { return Err(Refusal::Resource("groups")); }
                budget.charge(map_cost(seeds.len()))?;
                let lane = seeds.get_mut(&run.lane).ok_or(Refusal::InvalidSupport)?;
                let end = run.first_seq.checked_add(run.events).ok_or(Refusal::InvalidSupport)?;
                if run.first_seq >= end || lane.ranges.last().is_some_and(|r| r.end > run.first_seq || r.epoch > run.epoch) {
                    return Err(Refusal::InvalidSupport);
                }
                budget.reserve(&mut lane.ranges, 1)?;
                lane.ranges.push(EventRange { first: run.first_seq, end, epoch: run.epoch }); count += 1;
            }
        }
    }
    Ok(seeds)
}

#[derive(Clone, Copy)]
struct Position { before: u64, epoch: u32, sequence: u64, tb0: u64, tb1: u64 }
#[derive(Clone, Copy)]
enum GroupKind {
    Events { first: u64, end: u64 },
    Bracket { sequence: u64, tb1: u64 },
}
#[derive(Clone, Copy)]
struct Group { epoch: u32, kind: GroupKind, bounds: Bounds }
const NO_BOUNDS: Bounds = Bounds { lower: None, upper_drain: None, upper_bracket: None, upper: None };

fn unpositioned(summary: &mut OrderSummary, epoch: u32, budget: &mut Budget) -> Checked<()> {
    budget.charge(map_cost(summary.unpositioned.len()))?;
    let count = summary.unpositioned.entry(epoch).or_default();
    *count = count.checked_add(1).ok_or(Refusal::InvalidSupport)?; Ok(())
}
fn positions(brackets: &[&Bracket], order: Option<&LaneOrder>, summary: &mut OrderSummary,
    budget: &mut Budget) -> Checked<Vec<Position>> {
    let mut out = Vec::new(); let mut j = 0;
    if let Some(order) = order {
        for p in &order.brackets_at {
            budget.charge(1)?;
            while j < brackets.len() && (brackets[j].epoch, brackets[j].seq) < (p.epoch, p.bracket_seq) {
                unpositioned(summary, brackets[j].epoch, budget)?; j += 1;
            }
            let b = brackets.get(j).ok_or(Refusal::InvalidSupport)?;
            if (b.epoch, b.seq) != (p.epoch, p.bracket_seq)
                || out.last().is_some_and(|old: &Position| old.before > p.events_before) { return Err(Refusal::InvalidSupport); }
            // Every positioned bracket needs at least one observation group.
            if out.len() >= budget.limits.groups.saturating_sub(budget.groups) { return Err(Refusal::Resource("groups")); }
            budget.reserve(&mut out, 1)?;
            out.push(Position { before: p.events_before, epoch: p.epoch, sequence: p.bracket_seq, tb0: b.tb0, tb1: b.tb1 });
            j += 1;
        }
    }
    while j < brackets.len() { unpositioned(summary, brackets[j].epoch, budget)?; j += 1; }
    Ok(out)
}

fn bracket_group(p: Position, lower: &mut Option<u64>, out: &mut Vec<Group>, budget: &mut Budget) -> Checked<()> {
    *lower = Some(lower.map_or(p.tb0, |old| old.max(p.tb0)));
    budget.group(out, Group { epoch: p.epoch, kind: GroupKind::Bracket { sequence: p.sequence, tb1: p.tb1 },
        bounds: Bounds { lower: *lower, ..NO_BOUNDS } })
}

/// Merge finite event intervals with support thresholds. Neither event counts
/// nor gaps are expanded. Groups <= ranges + 2*positions + drains.
fn groups(ranges: &[EventRange], positions: &[Position], drains: &[Drain], created: Option<u64>,
    budget: &mut Budget) -> Checked<Vec<Group>> {
    let mut out = Vec::new(); let mut p = 0; let mut d = 0; let mut lower = created;
    for range in ranges {
        budget.charge(1)?;
        let mut cursor = range.first;
        while cursor < range.end {
            budget.charge(1)?;
            while p < positions.len() && positions[p].before <= cursor {
                bracket_group(positions[p], &mut lower, &mut out, budget)?; p += 1;
            }
            while d < drains.len() && drains[d].events_through <= cursor { budget.charge(1)?; d += 1; }
            let mut end = range.end;
            if let Some(next) = positions.get(p) { end = end.min(next.before); }
            if let Some(next) = drains.get(d) { end = end.min(next.events_through); }
            if end <= cursor { return Err(Refusal::InvalidSupport); }
            budget.group(&mut out, Group { epoch: range.epoch, kind: GroupKind::Events { first: cursor, end },
                bounds: Bounds { lower, upper_drain: drains.get(d).map(|drain| drain.tb_copy), ..NO_BOUNDS } })?;
            cursor = end;
        }
    }
    while p < positions.len() { bracket_group(positions[p], &mut lower, &mut out, budget)?; p += 1; }
    let mut upper: Option<u64> = None;
    for group in out.iter_mut().rev() {
        budget.charge(1)?;
        if let GroupKind::Bracket { tb1, .. } = group.kind { upper = Some(upper.map_or(tb1, |old| old.min(tb1))); }
        group.bounds.upper_bracket = upper;
        group.bounds.upper = match (group.bounds.upper_drain, upper) {
            (Some(a), Some(b)) => Some(a.min(b)), (a, b) => a.or(b),
        };
    }
    Ok(out)
}

fn first_id(group: Group) -> ObservationId {
    match group.kind { GroupKind::Events { first, .. } => ObservationId::Event(first),
        GroupKind::Bracket { sequence, .. } => ObservationId::Bracket { epoch: group.epoch, sequence } }
}
fn last_id(group: Group) -> ObservationId {
    match group.kind { GroupKind::Events { end, .. } => ObservationId::Event(end - 1),
        GroupKind::Bracket { sequence, .. } => ObservationId::Bracket { epoch: group.epoch, sequence } }
}
fn contradiction(summary: &mut OrderSummary, a: Group, b: Group, previous: ObservationId, next: ObservationId,
    budget: &mut Budget) -> Checked<()> {
    if let (Some(lower), Some(upper)) = (a.bounds.lower, b.bounds.upper) {
        if lower > upper {
            for epoch in [a.epoch, b.epoch] {
                budget.charge(map_cost(summary.contradicted_epochs.len()))?;
                summary.contradicted_epochs.insert(epoch);
            }
            if budget.findings < budget.limits.finding_rows {
                budget.reserve(&mut summary.witnesses, 1)?;
                summary.witnesses.push(ContradictionWitness { previous, next, lower, upper }); budget.findings += 1;
            }
        }
    }
    Ok(())
}
fn check_order(groups: &[Group], summary: &mut OrderSummary, budget: &mut Budget) -> Checked<()> {
    for (i, &group) in groups.iter().enumerate() {
        budget.charge(1)?;
        if let GroupKind::Events { first, end } = group.kind {
            if end - first >= 2 {
                contradiction(summary, group, group, ObservationId::Event(first), ObservationId::Event(first + 1), budget)?;
            }
        }
        if i > 0 {
            let previous = groups[i - 1];
            if group.epoch < previous.epoch { summary.observation_epochs_decrease = true; }
            contradiction(summary, previous, group, last_id(previous), first_id(group), budget)?;
        }
    }
    Ok(())
}

fn push_reason(reasons: &mut Vec<Refusal>, reason: Refusal, budget: &mut Budget) -> Checked<()> {
    budget.reserve(reasons, 1)?; reasons.push(reason); Ok(())
}
fn source_reasons(source: &super::SourceWindow, order: Option<&LaneOrder>, summary: &OrderSummary,
    budget: &mut Budget) -> Checked<Vec<Refusal>> {
    let mut reasons = Vec::new(); budget.charge(7)?;
    if source.structure != Structure::Complete { push_reason(&mut reasons, Refusal::SourceIncomplete, budget)?; }
    if source.prefix_lost != 0 || source.retained_lost != 0 {
        push_reason(&mut reasons, Refusal::SourceLoss { prefix: source.prefix_lost, retained: source.retained_lost }, budget)?;
    }
    if source.protocol_error.is_some() { push_reason(&mut reasons, Refusal::ProtocolError, budget)?; }
    if !matches!(source.open_begins, OpenBegins::Verified(_)) { push_reason(&mut reasons, Refusal::OpenCountUnverified, budget)?; }
    if !source.raw_valid && reasons.is_empty() { push_reason(&mut reasons, Refusal::InvalidSource, budget)?; }
    if order.is_none() { push_reason(&mut reasons, Refusal::NoOrder, budget)?; }
    if !summary.final_present { push_reason(&mut reasons, Refusal::NoOrderFinal, budget)?; }
    if let Some(dropped) = summary.dropped.filter(|&n| n != 0) { push_reason(&mut reasons, Refusal::OrderDropped(dropped), budget)?; }
    if summary.first_tb_decrease.is_some() { push_reason(&mut reasons, Refusal::DrainTbDecrease, budget)?; }
    if summary.observation_epochs_decrease { push_reason(&mut reasons, Refusal::ObservationEpochOrder, budget)?; }
    Ok(reasons)
}

fn endpoints(source: &super::SourceWindow, groups: &[Group], order: &OrderSummary, base: &[Refusal],
    reviewed: bool, budget: &mut Budget) -> Checked<Vec<Endpoint>> {
    let mut out = Vec::new(); budget.reserve(&mut out, source.events.len())?;
    let mut g = 0usize; let mut previous = None;
    for event in &source.events {
        budget.charge(1)?;
        if previous.is_some_and(|p| p >= event.seq) { return Err(Refusal::InvalidSupport); } previous = Some(event.seq);
        while g < groups.len() && match groups[g].kind { GroupKind::Bracket { .. } => true,
            GroupKind::Events { end, .. } => end <= event.seq } { budget.charge(1)?; g += 1; }
        let group = groups.get(g).ok_or(Refusal::InvalidSupport)?;
        if group.epoch != event.epoch || !matches!(group.kind, GroupKind::Events { first, end } if first <= event.seq && event.seq < end) {
            return Err(Refusal::InvalidSupport);
        }
        let bounds = group.bounds;
        let mut reasons = Vec::new(); budget.reserve(&mut reasons, base.len())?; reasons.extend_from_slice(base);
        budget.charge(map_cost(order.unpositioned.len()) + map_cost(order.contradicted_epochs.len()))?;
        if let Some(&count) = order.unpositioned.get(&event.epoch).filter(|&&n| n != 0) {
            push_reason(&mut reasons, Refusal::UnpositionedBrackets { epoch: event.epoch, count }, budget)?;
        }
        if order.contradicted_epochs.contains(&event.epoch) {
            push_reason(&mut reasons, Refusal::CausalOrderContradiction { epoch: event.epoch }, budget)?;
        }
        if bounds.lower.is_none() { push_reason(&mut reasons, Refusal::MissingLower, budget)?; }
        if bounds.upper.is_none() { push_reason(&mut reasons, Refusal::MissingUpper, budget)?; }
        if matches!((bounds.lower, bounds.upper), (Some(l), Some(u)) if l > u) { push_reason(&mut reasons, Refusal::EmptyInterval, budget)?; }
        let placement = if !reasons.is_empty() { Placement::Unavailable(reasons) }
            else if reviewed { Placement::QualifiedCausal { lower: bounds.lower.unwrap(), upper: bounds.upper.unwrap() } }
            else { Placement::ConditionalCausal { lower: bounds.lower.unwrap(), upper: bounds.upper.unwrap() } };
        out.push(Endpoint { seq: event.seq, epoch: event.epoch, bounds, placement });
    }
    Ok(out)
}

fn evaluate(retained: &Retained, seeds: BTreeMap<u32, Seed>, classification: Option<&Classification>, budget: &mut Budget)
    -> Checked<(BTreeMap<u32, Lane>, Option<Classification>)> {
    let mut count = 0usize;
    for source in retained.sources.values() {
        budget.charge(1)?;
        count = count.checked_add(source.events.len()).ok_or(Refusal::Resource("endpoint rows"))?;
        if count > budget.limits.endpoint_rows { return Err(Refusal::Resource("endpoint rows")); }
    }
    // Charge every retained profile byte before cloning the contextual result.
    if let Some(c) = classification {
        for text in c.failures.iter().chain(c.not_checked.iter()) { budget.charge(text.len() as u64 + 1)?; }
        budget.charge(1)?;
    }
    let classification = classification.cloned();
    let reviewed = classification.as_ref().is_some_and(Classification::holds);
    let mut by_domain: BTreeMap<u32, Vec<&Bracket>> = BTreeMap::new();
    for bracket in &retained.brackets {
        budget.charge(map_cost(by_domain.len()))?;
        let list = by_domain.entry(bracket.domain_id).or_default();
        if list.last().is_some_and(|old| (old.epoch, old.seq) >= (bracket.epoch, bracket.seq)) { return Err(Refusal::InvalidSupport); }
        budget.reserve(list, 1)?; list.push(bracket);
    }
    let mut lanes = BTreeMap::new();
    for (&key, source) in &retained.sources {
        let SourceKey::Lane(id) = key else { return Err(Refusal::InvalidSupport); };
        budget.charge(map_cost(seeds.len()))?;
        let seed = seeds.get(&id).ok_or(Refusal::InvalidSupport)?;
        budget.charge(map_cost(retained.order.as_ref().map_or(0, |o| o.lanes.len())) + map_cost(by_domain.len()))?;
        let order = retained.order.as_ref().and_then(|o| o.lanes.get(&id));
        let mut summary = OrderSummary { final_present: order.is_some_and(|o| o.final_.is_some()),
            dropped: order.and_then(|o| o.final_.map(|f| f.1)), first_tb_decrease: order.and_then(|o| o.tb_decrease),
            unpositioned: BTreeMap::new(), contradicted_epochs: BTreeSet::new(), observation_epochs_decrease: false, witnesses: Vec::new() };
        let brackets = by_domain.get(&seed.domain).map_or(&[][..], Vec::as_slice);
        let positions = positions(brackets, order, &mut summary, budget)?;
        let groups = groups(&seed.ranges, &positions, order.map_or(&[][..], |o| &o.drains), order.and_then(|o| o.created), budget)?;
        check_order(&groups, &mut summary, budget)?;
        let reasons = source_reasons(source, order, &summary, budget)?;
        let endpoints = endpoints(source, &groups, &summary, &reasons, reviewed, budget)?;
        budget.charge(map_cost(lanes.len()))?;
        lanes.insert(id, Lane { domain_id: seed.domain, order: summary, endpoints });
    }
    Ok((lanes, classification))
}

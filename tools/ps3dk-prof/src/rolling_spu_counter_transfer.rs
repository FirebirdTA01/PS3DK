//! Pure transfer for an already stable SPU observation sequence.
//! This does not establish support stability, PROFILE qualification, structural
//! validity, or any runtime capability. No synthetic observations are produced.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Policy {
    pub rmax: u64,
    pub rate_num: u128,
    pub rate_den: u128,
    pub accumulator_max: u128,
}
impl Default for Policy {
    fn default() -> Self {
        Self { rmax: 2, rate_num: 1, rate_den: 1, accumulator_max: u128::MAX }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Position {
    Event { seq: u64 },
    Bracket { before: u64 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Observation {
    pub epoch: u32,
    pub position: Position,
    pub raw: u32,
    pub lower: Option<u64>,
    pub upper: Option<u64>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StepClass { NoBound, OrderContradiction, DContradicted, LongGap }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Boundary { Start, Epoch, Loss, Step(StepClass) }

/// Fixed-size state for the current segment, not an evicted event inventory.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Summary {
    pub last: Option<Observation>,
    pub segment_id: u64,
    pub last_boundary: Option<Boundary>,
    pub proven_steps: u64,
    pub ticks: Option<u128>,
    pub rounded_steps: Option<u128>,
    pub forward_lower: Option<u128>,
    pub range_bad: bool,
    pub rmax1_bad: bool,
}

/// Privately constructed. No unvalidated import/serialization is exposed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Transfer {
    policy: Policy,
    summary: Summary,
}
impl Transfer {
    pub fn summary(&self) -> Summary { self.summary }
}

#[derive(Debug, Clone, Copy)]
pub struct Limits {
    pub observations: usize,
    /// Structural work: 4 units per advance plus 32 per actual observation,
    /// or 8 for backward. Charged before work; not a wall-clock budget.
    /// The caller also owns a cumulative outer bound across calls.
    pub work: u64,
}
impl Default for Limits {
    fn default() -> Self { Self { observations: 4096, work: 1 << 20 } }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Feasibility { Consistent, Contradiction, UnavailableRange }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    UnsupportedPolicy,
    PolicyMismatch,
    Invalid(&'static str),
    Arithmetic(&'static str),
    Limit(&'static str),
}

fn check_policy(policy: Policy) -> Result<(), Error> {
    if policy.rmax != 2 || policy.rate_num == 0 || policy.rate_den == 0 {
        return Err(Error::UnsupportedPolicy);
    }
    Ok(())
}

fn position(observation: Observation) -> Result<(u64, u64), Error> {
    match observation.position {
        Position::Event { seq } => Ok((seq, seq.checked_add(1).ok_or(Error::Invalid("event sequence overflow"))?)),
        Position::Bracket { before } => Ok((before, before)),
    }
}

/// Same D predicates/order as the full-source evaluator. This pure surface is
/// deliberately independent of its Analysis/profile/runtime dependencies.
fn step(previous: Observation, next: Observation) -> Option<StepClass> {
    let (Some(lower), Some(upper)) = (previous.lower, next.upper) else { return Some(StepClass::NoBound); };
    if upper < lower { return Some(StepClass::OrderContradiction); }
    // RMAX is admitted as exactly 2; the product fits u128 for all u64 bounds.
    let bound = 2 * u128::from(upper - lower);
    let modular = u128::from(previous.raw.wrapping_sub(next.raw));
    if modular > bound { Some(StepClass::DContradicted) }
    else if bound >= 1u128 << 32 { Some(StepClass::LongGap) }
    else { None }
}

fn rounded_step(modular: u32, policy: Policy) -> Result<u128, Error> {
    let product = u128::from(modular).checked_mul(policy.rate_den)
        .ok_or(Error::Arithmetic("RMAX1 step multiplication"))?;
    // Do not implement ceil as (product + numerator - 1) / numerator: that
    // overflows for a valid ratio whose product is near the type maximum.
    (product / policy.rate_num).checked_add(u128::from(product % policy.rate_num != 0))
        .ok_or(Error::Arithmetic("RMAX1 step ceiling"))
}

fn check_bounds(state: &mut Summary, observation: Observation) {
    if state.range_bad { return; }
    if state.forward_lower.is_some_and(|lower| lower > u128::from(u64::MAX)) {
        state.rmax1_bad = true;
    }
    if let Some(upper) = observation.upper.map(u128::from) {
        // The forward maximum covers every earlier explicit lower constraint.
        // T also protects the implicit nonnegative time at the segment start.
        if state.forward_lower.is_some_and(|lower| lower > upper)
            || state.rounded_steps.is_some_and(|total| total > upper) {
            state.rmax1_bad = true;
        }
    }
}

fn observe(state: &mut Summary, observation: Observation, policy: Policy) -> Result<(), Error> {
    if observation.epoch == 0 { return Err(Error::Invalid("zero observation epoch")); }
    let (first, _) = position(observation)?;
    let boundary = if let Some(previous) = state.last {
        if observation.epoch < previous.epoch { return Err(Error::Invalid("observation epoch regression")); }
        let (_, previous_next) = position(previous)?;
        if first < previous_next { return Err(Error::Invalid("observation position regression")); }
        if observation.epoch != previous.epoch { Some(Boundary::Epoch) }
        else if first > previous_next { Some(Boundary::Loss) }
        else { step(previous, observation).map(Boundary::Step) }
    } else { Some(Boundary::Start) };

    if boundary.is_some() {
        state.segment_id = state.segment_id.checked_add(1).ok_or(Error::Arithmetic("segment count"))?;
        state.proven_steps = 0;
        state.ticks = Some(0);
        state.rounded_steps = Some(0);
        state.forward_lower = observation.lower.map(u128::from);
        state.range_bad = false;
        state.rmax1_bad = false;
    } else {
        let previous = state.last.ok_or(Error::Invalid("missing segment predecessor"))?;
        let modular = previous.raw.wrapping_sub(observation.raw);
        state.proven_steps = state.proven_steps.checked_add(1).ok_or(Error::Arithmetic("proven step count"))?;
        if !state.range_bad {
            let old_ticks = state.ticks.ok_or(Error::Invalid("missing segment coordinate"))?;
            state.ticks = old_ticks.checked_add(u128::from(modular)).filter(|&f| f <= policy.accumulator_max);
            if state.ticks.is_none() {
                // A physical cut cannot turn this unavailable segment into a
                // fresh zero-based one. A genuine boundary may start anew.
                state.range_bad = true;
                state.rounded_steps = None;
                state.forward_lower = None;
            } else {
                let minimum_elapsed = rounded_step(modular, policy)?;
                let old_total = state.rounded_steps.ok_or(Error::Invalid("missing rounded step sum"))?;
                state.rounded_steps = Some(old_total.checked_add(minimum_elapsed)
                    .ok_or(Error::Arithmetic("RMAX1 rounded step sum"))?);
                let inherited = state.forward_lower.map(|lower| lower.checked_add(minimum_elapsed)
                    .ok_or(Error::Arithmetic("RMAX1 forward sum"))).transpose()?;
                state.forward_lower = match (inherited, observation.lower.map(u128::from)) {
                    (Some(a), Some(b)) => Some(a.max(b)),
                    (a, b) => a.or(b),
                };
            }
        }
    }
    state.last = Some(observation);
    state.last_boundary = boundary;
    check_bounds(state, observation);
    Ok(())
}

/// Continue the current segment using actual observations whose order/bounds
/// are already stable. A physical checkpoint cut does not create a boundary.
pub fn advance(
    prefix: Option<&Transfer>,
    policy: Policy,
    observations: &[Observation],
    limits: Limits,
) -> Result<Transfer, Error> {
    if observations.len() > limits.observations { return Err(Error::Limit("observation count")); }
    let count = u64::try_from(observations.len()).map_err(|_| Error::Limit("observation count"))?;
    let work = count.checked_mul(32).and_then(|n| n.checked_add(4)).ok_or(Error::Limit("transfer work"))?;
    if work > limits.work { return Err(Error::Limit("transfer work")); }
    check_policy(policy)?;
    let mut result = if let Some(prefix) = prefix {
        if prefix.policy != policy { return Err(Error::PolicyMismatch); }
        prefix.clone()
    } else {
        Transfer { policy, summary: Summary { last: None, segment_id: 0, last_boundary: None,
            proven_steps: 0, ticks: None, rounded_steps: None, forward_lower: None,
            range_bad: false, rmax1_bad: false } }
    };
    // Only this local fixed-size copy changes. A refusal exposes no partially
    // advanced state and leaves the caller's prefix available unchanged.
    for &observation in observations { observe(&mut result.summary, observation, policy)?; }
    Ok(result)
}

/// Add a translated RMAX1 constraint at the LAST current observation only.
/// This does not change old causal bounds or certify a late-U2 stability guard.
pub fn backward(
    prefix: &Transfer,
    boundary_upper: Option<u64>,
    limits: Limits,
) -> Result<Feasibility, Error> {
    if limits.work < 8 { return Err(Error::Limit("backward work")); }
    let state = prefix.summary;
    let Some(last) = state.last else {
        return if boundary_upper.is_some() { Err(Error::Invalid("upper constraint without an observation")) }
            else { Ok(Feasibility::Consistent) };
    };
    if state.range_bad { return Ok(Feasibility::UnavailableRange); }
    if state.rmax1_bad { return Ok(Feasibility::Contradiction); }
    let upper = match (last.upper, boundary_upper) {
        (Some(a), Some(b)) => Some(a.min(b)),
        (a, b) => a.or(b),
    };
    if let Some(upper) = upper.map(u128::from) {
        if state.forward_lower.is_some_and(|lower| lower > upper)
            || state.rounded_steps.is_some_and(|total| total > upper) {
            return Ok(Feasibility::Contradiction);
        }
    }
    Ok(Feasibility::Consistent)
}

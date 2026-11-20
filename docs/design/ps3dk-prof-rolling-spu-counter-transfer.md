# Stable SPU counter transfer

This pure unit preserves the current decrementer lift segment across a
checkpoint cut. It does not enable rolling capture, import a checkpoint,
validate late support, or qualify a hardware profile.

Implementation: `tools/ps3dk-prof/src/rolling_spu_counter_transfer.rs`.
Tests: `tools/ps3dk-prof/tests/rolling_counter_transfer.rs`.

## Preconditions and interface

The caller supplies actual observations already in stable lane program order,
with stable causal bounds. It owns original framing, source identities,
calibration/ORDER matching, source final validity and PROFILE classification.
The transfer cannot establish those preconditions from its scalar state.

`advance(prefix, policy, observations, limits)` returns a privately constructed,
constant-size `Transfer`. `advance(None, ...)` starts empty history. An existing
prefix binds the exact policy, including its accumulator limit; a policy change
rejects even on an empty advance. An empty advance under the same policy is
identity. Errors expose no partially advanced state and leave the supplied
prefix unchanged.

The admitted policy has RMAX 2 and a positive rational RMAX1 rate `(num, den)`.
The observation position is either event `[seq, seq+1)` or bracket
`[events_before, events_before)`. Event sequence overflow, epoch zero/regression
and positions before the previous next-position reject. A genuine epoch change
or source-sequence gap starts a new segment; a physical storage cut does not.

For an adjacent same-epoch, gap-free pair, compute the down-counter modular
advance `m=(previous.raw-current.raw) mod 2^32`. Apply these predicates in order:

1. Missing previous lower/current upper: NoBound.
2. Current upper below previous lower: OrderContradiction.
3. `m > 2*(upper-lower)`: DContradicted.
4. `2*(upper-lower) >= 2^32`: LongGap.
5. Otherwise: Proven, including equality at the D bound and a zero advance.

Each non-Proven edge starts a genuine new segment. This classification does not
invent a reset epoch from a raw jump or interpret zero advance as execution time.

## Constant-size transfer

The state retains the policy, last actual observation, segment identity and last
boundary, proven-step count, and three current-segment coordinates:

- `F`: checked cumulative modular ticks.
- `T`: sum of per-edge `ceil(m*den/num)` minimum elapsed steps.
- `A`: optional propagated lower bound, `max(actual_lower, previous_A + step)`
  over present operands.

F starts at zero. Exceeding the bound in `accumulator_max` makes the segment
range-unavailable; F/T/A become unavailable until a genuine new segment. A cut
cannot reset this state. Multiplication and T/A addition overflow return an
explicit arithmetic error. Ceiling division uses quotient and remainder rather
than an overflowing `product + num - 1` expression.

Within an available segment, A outside the u64 timestamp range, A above an
actual upper, or an actual upper below T makes the RMAX1 contradiction sticky.
An RMAX1 contradiction does not erase the modular tick count. Segment boundaries
reset segment-local failures; source/epoch-wide gates remain the caller's job.

`backward(prefix, boundary_upper, limits)` adds an upper constraint at the last
current observation. Combine it with that observation's actual upper. The
result is Consistent only when existing checks hold and the combined upper is
at least A, when present, and T. These conditions respectively preserve earlier
lower constraints and nonnegative time at the segment's start. Range-unavailable
state reports UnavailableRange. A supplied upper on empty history rejects.

This operation is constraint elimination at an existing boundary. It does not
rewrite old causal bounds, reclassify old D edges or insert observations.

## Limits and qualification boundary

Advance reserves 4 structural work units plus 32 per actual input observation
before traversal; backward reserves 8. The input observation count has a
separate limit. State storage is constant and no historical event vector is
allocated. These are per-call bounds; the caller must charge a cumulative budget
across checkpoint and segment operations. They are not wall-clock deadlines.

BRACKET_AT may arrive after events and insert a genuine observation into their
program order. Later positioned calibration can also change earlier upper
bounds. A separate observation-position fence, unresolved-tail policy and
future-upper stability proof are required before compacting such history. This
unit provides none of them. Its backward constraint is not a substitute.

Consistent does not mean reviewed ticks, calibrated placement, valid source
finals or measured duration. Complete retained pairs, source losses, causal
contradictions, censored boundaries and the exact reviewed SPU profile remain
separate obligations. No counter ownership or decrementer write is authorized.

## Validation and sensitivity

The tests compare transfer state and every cut against independently expanded
forward/backward bounds over 61,320 finite traces. Explicit controls cover D and
LongGap equality, wrap, missing bounds, epoch/LOSS boundaries, rational rounding,
range state, checked arithmetic, policy binding and exact resource boundaries.

`tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py` runs an unchanged
15-test baseline and 18 isolated source mutants. Each mutant must compile and
fail its named test at the specified assertion line. Compiler failure, unrelated
panic, missing tests, crash, timeout and log overflow fail the sensitivity gate.
The runner uses a new output directory, serial jobs, input hashes, bounded logs
and process deadlines; it never edits the checkout. Compiler scheduling remains
the caller's responsibility. `--check-only` validates pins and substitutions
without writing or executing subprocesses.

Two redundancies are intentional. Reachable multi-observation Proven segments
have A>=T, so removing only `V>=T` is not independently detectable while `V>=A`
remains. T-addition overflow is also caught by checked A addition on such states;
the sensitivity control changes that pair together and separately tests an
A-only overflow where T still fits. These controls make no claim about an
untrusted state importer or dynamic late-support handling.

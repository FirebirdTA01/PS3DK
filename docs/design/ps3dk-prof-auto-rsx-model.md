# Automatic RSX observation: host model

This is a host specification and test gate, based on the emitters at
`60fe5924092ccbdc3f61e681fd558d056146941a`. It does not instrument the SDK,
allocate a capture flag or chunk type, change a public API, or implement a
production reader or viewer. No target ordering, timestamp rate, coherent
report read, utilization or bottleneck classification follows from this model.

## Covered operations and preserved application work

The initial population is the canonical safe `rsxDrawVertexArray`,
`rsxDrawIndexArray` and `rsxClearSurface` operations. A Cell alias delegates
once to that seam. Unsafe, immediate and CALL forms are named exclusions.
Direct packets and uninstrumented providers have UNKNOWN coverage; their
absence from a trace is never evidence of no work.

Observation is admitted only when the complete operation plus 12 marker
words fits in the current segment, and a report pair and metadata credit
are available. Admission does not allocate, flush, poll, call a sink or
invoke a wrap callback. Refusal calls the application emitter exactly once
and preserves its result. An admitted operation adds 48 command bytes and
uses two 16-byte report slots. These words may increase later wrap frequency.

Sizing is independent O(1) arithmetic, checked against the three actual
emitter bodies. The fixture checks a pinned UTF-8/LF source digest before
extracting them and records both raw source and body hashes. Its reservation
macro records the original `CURRENT_BEGIN` groups; replay must preserve
those groups, not split them into individual method packets. A six-word
clear with five words left therefore wraps before the entire clear. The
existing two-phase FIFO protocol supplies the fake-port wrap behavior.
The buffer has an additional reserved tail-JUMP word before its canary.

Supported draw counts are 1 through 0xFFFFFF, with both declared index
widths. Unsupported arguments are counted and delegated; tests use an
opaque stand-in instead of executing invalid legacy inputs. They do not
claim those inputs are safe in the SDK. Exact word projection tests use
the real legal emitters, including misalignment and batch boundaries.
Clear's existing WAIT_FOR_IDLE remains part of its application words.

## Ownership and evidence lifetime

The modeled lifecycle is FREE, CLAIMED, EMITTED, PUBLISHED, COMPLETE and
RETIRED. QUARANTINE retains uncertain emission, regressed timestamps and
outstanding ownership at STOP. Neither an encoding offer nor a failed or
short sink write retires a ticket. Both explicit report-completion facts
and whole-record sink acceptance are required. The ticket retains its
operation kind and frame identity. A closed or absent frame routes later
operations to the explicit unassociated bucket.

Publication is an explicit owner action, distinct from report completion.
Serials are nonzero and observation is refused before 32-bit serial wrap.
Generation, serial, ticket and report-half identity are checked. The fake
device supplies completion facts; changing a report value alone cannot
prove them. Out-of-order CPU observations of the two completed reports
are allowed; their final timestamp order must still be nondecreasing.

The shared registry rejects overlapping report ranges, label collisions,
duplicate owners and invalid ranges. Binding a failed claim suppresses
observation. Rebinding is allowed only before the first ticket; registry
release requires a stopped, matching generation with no outstanding
ownership. Fixtures without an external registry explicitly assume a
private range starting at report 0 and label 240. This assumption is not
an application resource-allocation policy.

The completion encoding is the existing six-word timestamp/label pair
used as a model input. Its target ordering and readability contract stays
open. The model's explicit publication event also does not implement the
future target seam for publication caused by an application flush or wrap.

The independent lifetime ledger explores every permutation of publication,
two report completions, whole acknowledgement, failed acknowledgement,
another application call and STOP, for pair pools 1/2/3 and metadata
capacities 0/1/2. Four additional schedules cover repeated failed writes,
continued application work and stale completion after reuse: 45,396
schedules and 318,087 transitions. This is a finite sequential state
machine check, not a C11 memory-order or arbitrary concurrency proof.

## Per-frame coverage

The host artifact reader retains ARRAY, INDEX and CLEAR visited, observed
and excluded counts per `(owner, frame)`, with exclusive named reasons.
With no loss, it checks `visited = observed + sum(excluded)` separately for each kind.
Owner zero means unassociated; frame zero with a nonzero owner is valid.
Every retained frame is displayed, including frames with zero observations.
Duplicate identities, malformed counters and inconsistent accounting are
refused. Open/unassociated frames and a coverage-loss flag are INCOMPLETE.
Saturated/lost counters stay lower bounds; no exclusion-rate decision is
derived from them.
The fixed frame table is a model limit, not a shipping retention policy.

Here, observed means that both marker commands were emitted. Admission
counts do not establish report readiness, sink acceptance or valid timing.
The reader states that distinction and always labels direct/uninstrumented
population UNKNOWN. The production reader must retain this per-frame,
per-kind coverage when its wire format is reviewed.

The dense control uses a total 1 MiB command allocation, modeled prefixes
of 4 and 4096 bytes, and a reserved four-byte tail JUMP. Neither prefix is
claimed to match Mossbrook's firmware layout. Each scenario has 64 frames
of 4096 mixed array/index/clear operations, with counts 1, 257, 4096 and
524033. Immediate report retirement isolates room pressure. A separate
blocked-sink case isolates ownership pressure; it must not be mislabeled
as a room failure. Exact counts and all frame rows are emitted to
`coverage.json`. These are fixture coverage measurements, not console
timings or a measured application population.

The host review tripwire is a room exclusion rate of at least 1% in any
complete fixture frame. It is not a release threshold. Even below it,
stage 2 must review a way to reserve the operation plus 12 words before
emission using the existing callback, with legal tail-JUMP placement and
unchanged application behavior. Operations larger than an extent require
a separate coverage design. Frequent report-credit exclusions instead
require drain-capacity and bounded-storage work; extra FIFO room cannot
repair them. No low-coverage fixture is accepted as delivery of the
one-option automatic profiler.

## Wait grammar and source inventory

The wait model is one owner/context stack with bounded record and depth
storage. Records retain actor, context, reason, domain, parent, frame token,
raw target/value and outcome. CPU_POLL and CPU_CALL_OPAQUE retain wall-time
intervals. GPU_COMMAND retains a location with UNKNOWN wait duration.
OBSERVER_INTERNAL retains overhead but is excluded from application totals.
The CPU summary is the union of known CPU intervals minus known internal
intervals, in either nesting direction. It does not add parent and child
inclusive times. It is not a GPU execution total.

Missing END is OPEN; STOP leaves it incomplete. A retained END without a
matching BEGIN is an orphan and latches loss. Overflow, wrong actor/parent,
clock regression and unsupported domains cannot produce an exact total.
The original wait call is outside this host grammar: no timeout, polling
condition or application return behavior is changed by these files.

The following inventory is source-scoped to the frozen base. All paths
are beneath `sdk/` unless stated otherwise. Nested reserve/wrap time must
remain separate from the reason-specific polling span.

| Source / function | Domain and actual condition | Bound, ownership and interception limit |
|---|---|---|
| `libgcm_cmd/src/ps3tc_fifo_wrap_protocol.h`, wrap callback | CPU_POLL: GET equals tail, then begin | Owner only; unbounded, reuse FIFO phase events. Fetch position is not rendering idle. |
| `libgcm_cmd/src/rsx_legacy/buffer_impl.h`, Finish | CPU_POLL: REF equality after reference emission and flush | Owner, unbounded, 30us sleep. Emission can wrap before the poll. |
| Same file, ResetCommandBuffer | CPU_POLL: GET equals 0x1000 after JUMP/PUT | Owner, unbounded, 30us sleep; separate reset-generation seam required. |
| `libgcm_cmd/src/rsx_legacy/commands_impl.h`, SetWaitLabel | GPU_COMMAND: four emitted words | No local CPU polling; reservation may nest a FIFO wait. |
| Same file, SetWaitForIdle and ClearSurface | GPU_COMMAND: two-word method; embedded in clear | Enqueue time is not GPU wait duration; do not double-count the clear. |
| `libgcm_sys_legacy/src/gcm_legacy_wrappers.c`, flip/wait imports | CPU_CALL_OPAQUE if timing the imported call | Firmware delegation, no local loop/bound. Getter timing alone does not identify an application poll loop. |
| `libPSGL/src/psgl_context.c`, psgl_wait_rsx_idle | CPU_POLL: fresh completion-label token | Owner, unbounded, 30us sleep; issue may wrap first. |
| Same file, psgl_context_wait_idle_bounded | CPU_POLL: fresh label or 2,000,000us deadline | Bound excludes reserve while issuing token; timeout retains resources. |
| Same file, psgl_context_swap | CPU_POLL: SetFlip retry, then psgl_ppu_too_far_ahead | Separate 1ms and 3ms sleeping loops, no local timeout; pacing needs additional evidence. |
| `libps3dkprof/src/rsx/prg.c`, prg_calibrate | OBSERVER_INTERNAL: label window | Caller deadline; reserve may wrap before polling deadline. Calibration overhead is not game work. |
| Same file, prg_detach / prg_detach_wait | OBSERVER_INTERNAL: ledger/final acknowledgement | Caller timeout/poll hook; includes sink/drainer progress, not just RSX progress. |
| `samples/profiling/heterogeneous-workload/source/scene.c`, scene_render | Application exclusion: callback + status + completion label | 2s timeout and 200us sleep; SDK getter instrumentation cannot recover the enclosing loop. |

Reason numbers in the test are local grammar identifiers, not wire IDs.
The source path/function in this inventory is their provenance. Target
interception must define a versioned reason map, full provider coverage
and frame/dependency association before these become user-facing evidence.

## Running and later gates

Run `auto-host-model-suite-test.py --out NEW_DIRECTORY --mutants`, then
the same command with a different directory and `--sanitize`. The suite
builds only host fixtures. Named mutants must fail their intended semantic
assertion; compilation failures and unrelated crashes do not count.

Target integration, Unsafe/immediate/CALL coverage, shared report-read
ordering, clock mapping, strict image/ELF identity, frame adapters, worker
cut skew and the Frame bottleneck page remain later reviewed work. This
model supplies no per-app calibration row and depends on no result from
the failed timer probe.

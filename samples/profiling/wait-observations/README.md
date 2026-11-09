# Automatic pthread wait observations

This diagnostic uses ordinary pthread calls and capture lifecycle calls, with
no manual zones or calls to internal wait hooks. Build it twice with
`PS3_PROF_WAITS=OFF` and `PS3_PROF_WAITS=MANUAL`, and the same pinned
`WAIT_BUILD_ID`. The observed variant requires the separately installed wait
archives. The default runtime remains unchanged.

A worker enters its user function before profiler initialization, confirmed by
a release/acquire handshake after its runtime start hook. It then explicitly
registers through the existing thread API. It makes 32 mutex calls, then holds a
distinct mutex acquired through a setup-only trylock. A release/acquire handshake
proves that it owns this mutex before main starts; it releases only after main
reports all 64 timed calls finished. The observed runtime ends its profiler
incarnation through the pthread cleanup adapter. The default
build explicitly ends that incarnation. Observed initialization registers its
calling thread as `init`, so a subsequent registration must return `EALREADY`;
the default build explicitly registers `main`. The main thread makes 2050 mutex calls,
64 timed mutex calls and 16 timed condition waits. Every timed mutex call uses
a fresh future deadline of 1 ms against the worker-held mutex and must return
ETIMEDOUT. Immediate success or EDEADLK fails the gate. The condition waits must
also return ETIMEDOUT; spurious success fails this fixed-count diagnostic rather than retrying.
No signal-to-waiter dependency is tested or claimed.

Immediate-success timed locks bypass the current observer at the initial trylock;
they are uncovered, not dropped events. This sample exercises the subsequent
kernel-wait path. Its setup trylock is outside the observed operation kinds.

The sample drains between blocks, ends registered threads, checks shutdown and
publishes a capture-specific `.wait.txt` receipt through checked write, flush,
sync, close and rename. Errors withhold that receipt. Static resources remain
owned by the process on early failure; there is no in-process recovery from an
uncertain producer. LV2 calls and thread join have no application-enforced bound:
the external run harness must enforce its own timeout and mark missing receipts
as harness failure.

The 82 timebase rows describe complete API-call loops, including ordinary mutex
unlocks, per-call timed deadlines, timeout duration, loop control and scheduling.
The timed rows are timeout workloads, not uncontended lock overhead. None of the
rows measures isolated instrumentation cost. The timer
uses `isync` around timebase reads; no stronger sampling-point claim is made.
Compare OFF/ON/OFF descriptively, report both OFF observations and the difference,
and retain raw ticks. Condition timeout duration is not CPU blocked time.

`tools/verify.py` checks the fixed receipt and the paired pinned reader's
`waits --json` output. The run manifest must independently bind capture, session,
SELF, reader and receipt hashes. ON requires 2162 retained returned calls,
conserved per-actor sequence counts, no application loss/refusal and known sites.
Profiler-internal exclusions must be present and are reported separately; they
are not application rows. OFF requires
no wait capability or observations. Pass `--expected-timeout` with ETIMEDOUT
derived from the paired target build's compiler and headers, and pin that value
and its derivation in the run manifest. Do not substitute the verifier host's
errno value. Run its host controls with `python host-tests/test_verify.py`.
The native host lifecycle control also executes the sample against the real
profiler core with a deterministic thread-start scheduling seam:
`python3 host-tests/test_lifecycle.py --out /tmp/wait-lifecycle-fresh`.
It substitutes the target timebase and sink path and does not validate LV2
adapters, target TLS, or instrumentation cost.

Hardware execution and overhead measurements require a separately reviewed run
plan. A successful build is not target validation or a release coverage claim.

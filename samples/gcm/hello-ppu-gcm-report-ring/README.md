# Report-ring feasibility probe

Design/build artifact for four hardware questions. This sample does not enable
report-ring capture in the profiler. A launch needs a reviewed, sealed packet
and explicit operator approval. A completed emulator run cannot establish the
hardware claims below.

## Known-good setup and deviations

The rendering baseline is `hello-ppu-cellgcm-state-tour`, which has completed on
the console. `source/state_tour_setup.h` copies six functions unchanged:
`local_align`, `init_rsx`, `init_shaders`, `init_geometry`, `bind_surface`, and
`env_reset`. The host gate checks their complete text and the five size macros
against that sample. CMake uses the original shader files directly.

Preserved: 64x64 offscreen local-memory A8R8G8B8 color and Z24S8 depth surfaces,
256-byte pitches, linear/single-sample descriptors, 64-byte local allocation
alignment, a four-vertex quad, position/color/UV bindings, 1x1 white local
texture and both shaders. The FIFO remains 0x40000 bytes and the host allocation
remains an 8 MiB, 1 MiB-aligned `memalign` allocation. Library selection matches
the state tour. No display configuration or flip is added.

Every intentional deviation is listed here:

| Deviation | Reason |
| --- | --- |
| Replace the 15 state groups with a fixed white-quad baseline and timestamped draws | Isolate report/copy ordering without changing rendering setup |
| Add two 126-pair report banks in application indices 516..1019, with guards at 512..515 and 1020..1023 | Full and partial report-block copies without taking profiler-owned indices |
| Add a separate 1 MiB LV2 allocation mapped by `cellGcmMapMainMemory` | Guarded main-memory DMA destinations, using the existing GCM host-buffer helper |
| Add 4,032 bytes of local poison data after the original allocations | Wrong-source B1 control; original surface/shader/vertex offsets stay unchanged |
| Use labels 110..114; reset/read back all five before any fence | Prevent cross-process stale values from completing a wait |
| Add report, transfer, backend-label/wait and command-label commands | These are the operations under test; their ordering is not presumed correct |
| Reserve 6,000 words before each batch and verify actual room | Avoid a FIFO callback while the early-publication control holds a GPU gate |
| Replace unbounded finish/ordinary timeout exit with bounded label/reference/GET==PUT checks and terminal recovery state | Prevent a normal return while submitted work is pending |
| Keep GCM-visible allocations until process teardown | A known-idle fence does not itself remove IO mappings |
| Add append-only, fsync'd trial records and a cooperative deadline | Attribute a stalled step and bound measurement; logging can delay a cooperative check |
| Build with warnings as errors; optionally inject the existing run hook | Build verification and host outcome delivery, with the watchdog limitation below |

No zcull binding, zpass accumulation, flip, main-location report, new shader, raw
method encoding, guessed report address or GPU-reset operation is introduced.

## Addressability gate and baseline

The probe first renders white and requires an exact `0xffffffff` center pixel,
a nonzero increasing timestamp pair, and a successful ordinary local-color
copy to mapped main memory. It validates the CPU report mapping's contiguity
before sentinel writes. All five labels are stored to zero, ordered with `sync`
and read back at startup. Every subsequent target for each label increases;
stale and future values cannot satisfy equality. The reference fence also uses
a fresh value rather than accepting its previous process value.

Before **any report DMA**, every report pointer must be contiguous inside the
configured local-memory range, and `cellGcmAddressToOffset` must agree with its
offset within that range. A separate report mapping or an unavailable conversion
produces `ADDRESS_UNAVAILABLE`, all four claims `INCONCLUSIVE`, and a normal
idle exit with code 2. This only refuses the proposed ordinary LOCAL-to-MAIN
route; it does not prove that no other addressable route exists. No offset is
guessed and no transfer is attempted with the rejected report source.

## Trial matrix and controls

There are eight trials per candidate/control at lengths 1, 7 and 126 pairs:
48 trials per claim, 192 total if every prerequisite is satisfied. There are no
warm-up retries. B1 runs first; failure to qualify any B1 length stops its
dependent claims. Banks alternate between trials. Every trial starts idle,
pre-writes a new sentinel into all 512 application report slots and all used
readback/guard words, then uses strictly increasing label targets.

| Claim | Candidate | Sensitive control |
| --- | --- | --- |
| B1: report storage can be copied exactly to MAIN | Write reports; bounded full drain; save CPU bytes; copy reports; drain; compare every byte and guards | Copy the known, allocated poison source instead; mismatch must be caught |
| A1: report writes precede transfer reads | Timestamped draws; backend label + GPU wait; report copy | Copy sentinel reports before timestamped draws; compare with completed CPU report bytes |
| A2: transfer reads finish before local report reuse | Save old generation while idle; old copy; backend label + GPU wait; rewrite timestamps; new copy | Rewrite before the old copy, so old-generation comparison must detect the substitution |
| B2: publication implies stable CPU-visible copy | Copy; backend publication + GPU wait; PPU snapshot immediately on that label, before final drain | Command publication before a GPU gate; PPU snapshots sentinels, then releases the gate so the copy can finish |

A2 also requires a strictly newer generation and an exact new-generation copy.
B2 requires its later, fully drained copy to match the saved source; that later
read never repairs its frozen early snapshot. All trials validate report times
and guards. A1's control may be hidden by a platform's scheduling; no catch is
an inconclusive detector, never support. These deliberately broken controls
establish detector sensitivity, not a bound on the frequency of natural races.

Each claim/length requires all eight valid candidate trials, no counterexample,
and at least one caught control for `SUPPORTED_THIS_WORKLOAD`. Invalid reference
evidence aborts with `INVALID`; a valid candidate mismatch is `REFUTED`; missing
trials or an insensitive control is `INCONCLUSIVE`. Zero failures in eight
trials does not establish a zero-risk bound. No result here authorizes reuse in
CALL replay, SPU command writers or another pipeline/memory mapping.

## Bounds and exit policy

Measurement deadline: 120 seconds of monotonic time from `RRP_BEGIN`, including
setup. Individual label/reference waits are bounded by two seconds. The deadline
is checked **before** a completed label is accepted. Final draining has a separate
130-second absolute deadline, leaving cleanup headroom; it does not admit another
trial. All bounds are cooperative: firmware calls, callbacks, flush and file I/O
can delay the next check. No unconditional termination guarantee is claimed.

A normal exit requires the final backend label, a fresh reference command (the
same primitive used by `cellGcmFinish`) and GET==PUT. There are no probe flips or
zcull bindings pending. No mapped memory is freed by the sample. Exit codes:
0 = every series supported for this workload; 1 = refuted; 2 = inconclusive or
address unavailable; 3 = setup/reference/deadline failure with an idle exit.

On a publication/final-fence timeout or an uncertain pending state, print
`RRP_RECOVERY_REQUIRED`. Issue no further GPU commands, free nothing, never
return or exitspawn, and do not retry. The process remains in a terminal operator
recovery state; its sleep loop is deliberately outside measurement. There is
no approved GPU-quiesce fallback.

**Run-hook caveat:** the optional existing hook's watchdog can override this
terminal state: at the proposed host `--timeout 240`, it sends TIMEOUT and tries
to exitspawn the loader. That relaunch is not GPU recovery and may itself hang
after an RSX wedge. A host timeout alone does not stop the guest. A launch packet
must pin the hook and document this interaction and manual recovery procedure.
The unhooked build reports through TTY and the crumb file only.

Output is append-only `/dev_hdd0/tmp/report-ring-probe.crumb`, identified by the
`session` timestamp. Records include baseline pixels/times, actual source and
local-memory addresses, trial starts/results, per-length verdicts, and either
`RRP_END ... idle=1` or `RRP_RECOVERY_REQUIRED`. Missing records are not success.

## Build and checks

Use the normal PPU CMake toolchain; the probe supports ILP32 and LP64 builds.
No installed SDK files are modified. Run `bash tests/host/report-ring-probe-test.sh`
for the pure logic, three named mutants, setup-copy check and source guards.
The test is registered in CI. The reviewed launch packet separately seals the
source, SDK inputs, shaders, tools, hook, ELF/SELF and emulator evidence.

The required hardware facts remain A1 (report-before-copy), A2 (read-before-reuse),
B1 (addressable exact transfer without fault/hang) and B2 (publication/CPU
visibility). An emulator that exposes reports outside configured VRAM exercises
only the baseline and safe-refusal path. That limitation must be retained in
the launch review; it does not qualify any hardware claim.

# ps3dk-prof viewer: desktop profiler app

Status: design for review. Phase 1 (offline viewer) approved by the director
2026-11-01; phases 2 to 4 are planned, not approved for implementation.

## Goal

One desktop app, Windows and Linux, in which a developer opens a profile from
a running or finished PS3 app, sees where the time goes, sees which functions
are called most, compares PPU work with SPU and RSX activity, and later steps
through code. It replaces reading `ps3dk-prof report` text. The acceptance app
is `samples/profiling/heterogeneous-workload`.

## The rule every view follows

The viewer shows only what the production readers have validated. A `.prof`
loads through `ps3dk_prof::load`, a `.arcs` through the production arcs
reader (`arcs::read_arcs`), and a pair through `companions::match_companions`.
The viewer has no parser of its own, so a number on screen is a number
`ps3dk-prof check`, `check-arcs`, `check-pair` and `report` also produce.

- Each file's own verdict (COMPLETE, INCOMPLETE with its reasons, or
  MALFORMED with the rule broken) and, with two files, the pair verdict
  (COMPLETE FINAL PAIR, PROVISIONAL, or PAIR REFUSED with the reason) are all
  shown, separately, in the title bar. A MALFORMED file opens to its
  diagnosis only.
- A lane that is not MEASUREMENT VALID is drawn hatched, with its reasons in
  the lane header; it is excluded from statistics, exactly as in `report`.
- Censored spans (open or truncated at stop) are drawn with an open-ended
  cap at the edge of the data, not a measured endpoint, and are never given a
  duration. An unavailable duration (END before BEGIN) shows as unavailable,
  never as 0.
- Times are converted from timebase ticks with the capture's own CLOCKS
  frequency. Exact tick values stay available in tooltips. Without a usable
  CLOCKS record, time conversion is shown as unavailable and only ticks are
  displayed.
- SPU and RSX values recorded by apps are raw and uncalibrated today. Until a
  measured calibration exists, they appear only in phase 3's separate
  UNCALIBRATED lanes, never on the PPU time axis.

## Phase 1: offline viewer (approved)

Input: a `.prof` file, optionally its `.arcs` companion, from disk or pulled
from the console.

Views:

1. **Overview**: verdict, app, build id, SDK, compiler, ABI and workload tag
   from the header; clock frequency; per-lane table (thread name,
   incarnation, records, frames, validity and reasons); trailer counters
   (dropped, refused, unregistered producers).
2. **Timeline**: one row per lane; spans as bars stacked by depth; frame
   boundaries as markers; zoom (wheel, box select) and pan; a tooltip with
   zone name, `file:line`, inclusive and exclusive time, and frame. Clicking a
   span selects its zone everywhere.
3. **Zones**: a sortable table (instances, completed, censored, total, mean,
   p50, p90, p99), with the reader's "completed instances only" label shown
   where it applies. Selecting a row highlights its instances on the timeline.
4. **Frames**: elapsed time per frame as one chart, and each lane's
   attributed work W as a separate series or table, labelled with the
   reader's W note: W is a span's whole exclusive work counted in the frame
   it started in. It is not clipped to the frame, can exceed the frame's
   elapsed time, and is never summed across lanes. No utilization percentage
   is derived from W. Clicking a frame opens it in the timeline.
5. **Call counts** (frequency only, never time). This view needs a `.arcs`
   companion.
   - Certified counts only for a pair that `match_companions` accepts as
     FINAL (`VerifiedPair::is_final`). An accepted pair that is not final
     opens a separate view labelled PROVISIONAL: counts observed so far, with
     no completeness claim. A refused pair shows the refusal reason and no
     counts.
   - Shown: a function table by call count, and a caller/callee graph of the
     recorded arcs. This is not an observed call stack: recursion and shared
     callees appear as cycles and shared nodes, and no path totals or
     durations are invented. An arc edge is never linked to a timed zone.
   - Counter-limit and unknown-count incarnations keep the reader's flags.
   - Symbols: the user may point the viewer at the app's unstripped ELF. Its
     identity is checked with the rules `gmon.rs` already applies. The ELF
     build id must equal the capture's build id. The ABI comes from the
     capture identity, never from the ELF class or OPD width, because both
     PPU ABIs are ELF64. A wrong, absent or unverifiable id leaves raw PCs,
     with the reason shown.

**Console pull**: a Connect dialog takes the console IP, lists the
`.prof` and `.arcs` files in `/dev_hdd0/tmp` and `/dev_usb000` over the
webMAN FTP server, and downloads the selected ones. It only reads the
console: it never deletes, renames or uploads there.
- Network I/O runs off the UI thread, with a size limit, a timeout and a
  cancel button.
- A download goes to a distinct `.partial` file and is renamed into the local
  cache only when the transfer ends normally. Remote names are sanitized for
  the local file system, and an existing local file is never overwritten; a
  new name is chosen instead.
- The raw downloaded bytes are kept unchanged and parsed normally. A file
  fetched while the app is still writing it is a snapshot: FTP success never
  implies COMPLETE, and the reader's verdict decides.

**Structure**: a new crate, `tools/ps3dk-prof-viewer`, built on eframe 0.29
and `ps3dk-ui` like `sfo-gui`. The view logic (time mapping, span layout,
filtering, table sorting, symbolization, FTP listing parsing) lives in a
plain Rust module with no egui types, so it is unit tested; the egui layer
only draws it. The app is added to the Windows and Linux host-tools packages
in the same change that ships it.

**Tests**: unit tests on the view model, built from the same synthetic
captures the reader tests use:
- captures: valid, incomplete, censored, clock anomaly, missing CLOCKS,
  cross-frame spans, two concurrent lanes;
- pairs: FINAL, provisional, refused, refused for unregistered producers,
  unknown or counter-limit counts;
- ELF identity: match, wrong build, absent id, both ABIs;
- downloads: interrupted transfer, growing or torn capture.

One golden test loads a real hardware capture, hash-pinned and checked to be
safe to distribute (no machine paths or secrets), and compares the zone table
with `report`. FTP listing parsing is tested with recorded webMAN responses.
No hardware is needed for phase 1.

**Increments**, each reviewed before the next: (1) local-file model plus the
Overview and Zones views; (2) Timeline; (3) Frames; (4) Call counts with ELF
symbols; (5) console pull. No runtime adapters or wire-format changes in
phase 1.

## Phase 2: live connection (planned)

A TCP sink in `libps3dkprof` (the runtime already takes sink callbacks)
streams chunks to the viewer while the app runs; the viewer connects by
console IP and updates the timeline as chunks arrive. A live view is
labelled PROVISIONAL until the stream's trailer arrives, matching the
reader's progress-snapshot rules. Needs: the sink, a reconnect and backlog
policy, and an RPCS3 plus HEN measurement of overhead.

## Phase 3: PPU, SPU and RSX on one timeline (planned)

Lanes for SPU jobs and RSX work, fed by the SPU raw-clock transport now in
review and by GPU timestamps the app records. They show raw counters as
UNCALIBRATED until a hardware calibration probe has measured how the SPU
decrementer and RSX timestamps map to the PPU timebase. Only then are they
drawn on the shared time axis.

## Phase 4: debugging (planned)

The viewer gains a debug panel driven by the in-app GDB stub (D2) or by GDB
itself: breakpoints, step, registers, memory, beside the profile. On HEN the
debug syscalls returned 0x8001042c in the 2026-10-26 probe, so hardware
breakpoints and stepping are expected to need a debug-mode console. Whether
an app on HEN can serve its own register and memory reads to the stub is a
backend capability still to be established; the failed probe does not
promise it. RPCS3 supports stepping today through its own GDB server.

## Parity with the PS3-era tools (initial inventory)

Director 2026-11-01: match or beat every visualization and workflow of the
Windows tools licensed developers of the era used (a target manager, a
debugger, a performance analyser and GPU analysis tools), with a layout at
least as good. This section is an initial inventory built from public
sources. It is a target, not a statement of support, and it is not yet
exhaustive: the coverage gaps at the end are open items to resolve.

### Support axes

A feature can be delivered on three separate axes, and each row says which
it needs. One axis never counts as delivery on another.

- **H (host):** visualization and analysis on the PC of data already
  captured.
- **A (in-app, HEN):** the app is built with our libraries (for example
  `libps3dkprof` zones) and records its own data on a retail console with
  HEN.
- **X (external):** capture or debugging of an unmodified app, without
  rebuilding it. The director's goal includes PPU, SPU and RSX profiling this
  way. No X adapter exists. Debug firmware may be necessary for some X
  features, but it is not evidence that an adapter can be built; X is an
  investigation and acceptance track of its own (row X1).

### Columns

- **Evidence**: how well public material supports the legacy feature:
  "primary" (a talk or paper by the tool's makers or a studio using it),
  "secondary" (community write-ups), "unverified" (a located source that
  review could not open), or "inferred" (no source states it outright). The
  source list is kept with the project's private notes.
- **Today**: what exists and is tested now. "viewer" is this app; "CLI" is
  the `ps3dk-prof` command line; "local" is a tool that is not shipped;
  "none" is nothing yet. Hardware evidence so far is one instrumented sample
  on HEN: it establishes that sample's PPU timings and raw counters, not
  system-wide observability.
- **Plan**: the increment or phase that delivers it, or "unplanned".
- **Axes**: the proposed delivery routes (H, A, X), not proven or exclusive
  requirements. Other routes stay open until discovery settles them. "debug
  fw?" marks rows where the old tool needed, or is reported to have needed, a
  development unit.

Each row gets an acceptance criterion and an evidence link on its own card
when the director schedules it; the row ID is the stable reference.

### Profiling

| ID | Legacy feature | Evidence | Today | Plan | Axes |
| --- | --- | --- | --- | --- | --- |
| P1 | Frame-by-frame timing, frames selectable | primary | CLI (frame elapsed, W per lane); hardware: 120 s sample capture | inc 3 Frames | H, A; X open |
| P2 | Named user markers | secondary | viewer (Zones, inc 1) | inc 2 adds them to the timeline | H, A |
| P3 | PPU thread timeline | secondary | none (lanes listed in Overview) | inc 2 Timeline | H, A; X open |
| P4 | Call frequency per function | primary | CLI `check-arcs` validates arc files only | inc 4 Call counts | H, A |
| P5 | Instrumented call graph with absolute timing, all threads | primary | none | unplanned | H, A; X open |
| P6 | Statistical PC sampling, PPU | secondary | none | unplanned | X; debug fw? |
| P7 | Statistical PC sampling, SPU | inferred | none | unplanned | X; debug fw? |
| P8 | PPU syscall trace | secondary | none | unplanned | X |
| P9 | PPU, SPU and RSX on one calibrated time axis | unverified; inferred for the analyser | none | phase 3: raw UNCALIBRATED lanes are partial delivery; the row needs a measured calibration | H, A; X open |
| P10 | SPU occupancy by instrumented jobs (count, share of an SPU per frame) | unverified | none | phase 3 | H, A |
| P11 | Whole-SPU utilisation including unobserved work | inferred | none | unplanned until P10 defines interval, denominator, SPU allocation count and calibrated clock and coverage gates | X |
| P12 | PPU hardware performance counters | secondary | none | unplanned | X; debug fw? |
| P13 | RSX hardware counters | secondary; primary for counter display | none | unplanned | X; debug fw? |
| P14 | RSX time per frame | inferred | the sample records GPU timestamps in its CSV (uncalibrated), not in `.prof` | phase 3 | H, A |
| P15 | Live capture, ring-buffer and fill modes | secondary | file capture with verdicts for torn captures | phase 2 live; ring mode unplanned | H, A |
| P16 | Scripted capture (start, stop, trigger) for automated runs | secondary | automated validation and reporting only (`check`, `report`, `check-pair`); capture orchestration is local scripting | unplanned | H, A |
| P17 | Pipeline and stall analysis in disassembly | primary | none | unplanned | H |

### Debugging and target control

| ID | Legacy feature | Evidence | Today | Plan | Axes |
| --- | --- | --- | --- | --- | --- |
| D1 | Find and connect to consoles, several at once | secondary | local (`ps3dk-run`, one console by IP) | phase 2 connect | H, A |
| D2 | Load and run an executable | secondary | local (`ps3dk-run`) | phase 2 | H, A |
| D3 | Colour-coded TTY channels | secondary | none in the viewer | phase 2 | H, A |
| D4 | Serve files from the PC | secondary | none | unplanned | open |
| D5 | Reset and power control | inferred | none | unplanned | X; debug fw? |
| D6 | Kernel explorer (processes, threads, modules) | secondary | none | phase 4 | X open |
| D7 | Source, disassembly and mixed views | secondary | none | phase 4 | H plus a backend |
| D8 | Several PPU and SPU threads at once | secondary | none | phase 4 (SPU inspection only, director 10-26) | A or X; backend open |
| D9 | PPU and SPU registers, 128-bit vectors | secondary; vector display inferred | none | phase 4 | A or X; backend open on HEN |
| D10 | Memory, variables, watch, call stack | secondary | none | phase 4 | A or X; backend open on HEN |
| D11 | Conditional and counted breakpoints | secondary | none | phase 4 | X; A (cooperative, in-app) open: the HEN debug-syscall probe (0x8001042c) does not rule it out |
| D12 | Expression evaluator | secondary | none | phase 4 via GDB | H for constants; target values need a backend |
| D13 | Docking, multi-monitor layouts, custom fonts and colours | secondary | fixed sidebar layout | layout increment before phase 3 | H |

### GPU

| ID | Legacy feature | Evidence | Today | Plan | Axes |
| --- | --- | --- | --- | --- | --- |
| G1 | Live RSX command-buffer log | unverified | none | unplanned | A (instrumented app) or X; both open |
| G2 | Per-draw-call inspection, API-call profiling | primary | none | unplanned | A (app-side recording); X open |
| G3 | Video and main memory explorer | primary | none | unplanned | X open |
| G4 | Shader debugging with breakpoints and stepping | unverified | none | unplanned | X; debug fw? |
| G5 | Frame capture to the PC | unverified | none | unplanned | X; live video reportedly needed a development unit |

### Track X: external capture of unmodified apps

| ID | Item | Today | Plan |
| --- | --- | --- | --- |
| X1 | Establish which PPU, SPU and RSX data can be captured from an app that was not rebuilt, on HEN and on debug firmware, and build an adapter for what can | none | unplanned; needs a bounded discovery task and its own acceptance |

### Coverage gaps

Public sources say little about how DMA traffic, lock and sync waits, SPURS
traces and GPU frame capture were shown. Each is a named investigation gap;
any design there is our own.

### What we guarantee

Every capture the viewer opens carries the reader's verdict (COMPLETE,
INCOMPLETE with reasons, or MALFORMED), torn and censored data are labelled,
and exact ticks sit behind every converted number. Captures open without the
console, on Windows and Linux.

### Layout

The old debugger used docking, tabbed windows across monitors. The
viewer keeps its sidebar of pages for increments 1 to 4. Before phase 3 a
layout increment adds docking panes (timeline on top; statistics, call
counts and, later, source and registers below), saved layouts, and
selection that follows across panes: a zone picked in a table highlights on
the timeline and the reverse.

## Not in phase 1

Live streaming, SPU and RSX lanes, debugging, editing or rewriting captures,
and any metric the reader does not already compute.

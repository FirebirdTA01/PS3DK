# ps3dk-prof viewer: desktop profiler app

Status: design for review. Phase 1 (offline viewer) approved by the director
2026-11-01. Phase 2 (live connection) requested by the director the same
day; its design below is under review. Phases 3 and 4 are planned, not
approved for implementation.

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

## Phase 2: live connection (design for review)

Director 2026-11-01: watch a capture in the PC app while the app runs, over
a proper connection (not FTP polling); the in-app approach below was
approved in principle. A retail console with HEN has no system debug
agent, so the agent is a library the app links, and the viewer connects to
it by console IP. Full DEX support (external capture of unmodified apps) is
a separate later track (X1).

### Agent

An opt-in part of `libps3dkprof`: one PPU thread with a TCP listener that
serves the capture FILE the normal file sink is writing. It never touches
the drain, the rings or sink callbacks, so the drain has no synchronous
dependency on the viewer. That is not zero cost: the agent competes for
CPU, memory, the file system and disk I/O, and its cost is measured (see
Gates). The agent and the runtime are built uninstrumented, and the agent
thread never registers as a profiled producer.

**API and ownership.**
- `ps3dk_prof_live_start(const ps3dk_prof_live_config *)` is valid only
  after a successful `ps3dk_prof_init()` with the FILE sink, and before
  `ps3dk_prof_shutdown()`. Before init or after shutdown it returns a
  state error; a second start returns EALREADY; with a callback sink it
  returns ENOTSUP. The config holds the port (default 31750), the poll
  interval (default 10 ms, a proposed value to be measured), the send
  timeout, the handshake timeout and the capture cap.
- The file path and the capture identity are not supplied by the caller.
  `ps3dk_prof_init()` already fixes both: the path it opened exclusively
  and the init timebase value in its name. Init publishes them once, as an
  immutable record, before it returns; start copies that record. Nothing
  reads mutable runtime state afterwards.
- Network initialization belongs to the app (the sample shows it). If the
  socket cannot be created or bound, start fails and the capture is
  unaffected.
- The end of a capture comes from the runtime's EXISTING finalization
  record (`ps3dk_prof__finalization()`, `prof_finalization.h`), which the
  winning shutdown publishes with release/acquire ordering after the sink
  close and the drain-thread join. The agent treats EOF before that record
  as "wait for more data". Once the record exists, the file is closed, so
  the agent sends to the file's end and then END. END copies the record's
  `io_finalized`, `trailer_written`, `join_failed`, `shutdown_result` and
  `accepted_bytes`. `accepted_bytes` is the whole file's size only when
  `io_finalized` is 1; otherwise it is reported as the acknowledged count,
  never as a successful final size. The host tests inject this record; they
  create no second source of truth.
- Calls are caller-serialized: one controlling thread calls start and
  stop; the agent thread is internal.
- `ps3dk_prof_live_stop(graceful_us, join_us)` is called after shutdown.
  Graceful first: a viewer that is streaming keeps receiving until END has
  been sent or `graceful_us` runs out. Then cancellation: the agent closes
  the viewer without END (a partial download, never a success claim) and
  exits. The agent never blocks in a socket call (non-blocking sockets,
  polled at the poll interval), so cancellation takes effect within one
  poll. Stop then waits up to `join_us` for the thread to exit and joins
  it. On timeout it returns ETIMEDOUT and the agent's state stays
  allocated (static storage, never freed), so a thread still inside a file
  call never touches freed memory; calling stop again keeps waiting. LV2
  file calls are not known to be deadline-bounded, so the deadline bounds
  the wait, not the thread.
- While the agent runs, a stream that ends by error (peer gone, protocol
  violation, stall) returns it to LISTEN; a later viewer starts again from
  offset 0. A completed stream (END sent) also returns to LISTEN, so a
  viewer that connects after the run still gets the whole file and END.
  Only stop ends the agent.
- There is no automatic destructor. An app that exits without calling stop
  ends the thread with the process.

### Link protocol, version 1

Frames: an 8-byte header (u16 type, u16 flags = 0, u32 payload length,
little-endian), payload at most 65,536 bytes. Anything else closes the
link with a reason.

| State | Agent sends | Agent accepts | Leaves when |
|---|---|---|---|
| LISTEN | nothing | one connection | accepted |
| HANDSHAKE | nothing | `CLIENT_HELLO` (magic, protocol version) within 2 s | valid hello: STREAM; timeout or anything else: close, LISTEN |
| STREAM | `HELLO`, then `DATA` | `PING` (at most 4 per second) | finalization record and all bytes sent: FINAL; error: close |
| FINAL | `END` | nothing | sent: close, LISTEN |

- `HELLO`: protocol version, agent version, capture identity (the init
  timebase value, app name and build id from the published init record),
  and the file name. The identity is for display; v1 has no resume.
- `DATA`: u64 absolute offset, then up to 65,528 bytes. Offsets start at 0
  and are contiguous; offset plus length is checked for overflow and
  against the capture cap (default 1 GiB).
- `END`: `io_finalized`, `trailer_written`, `join_failed`,
  `shutdown_result` and `accepted_bytes` from the finalization record,
  plus the bytes this stream sent. It proves transport completion only.
- `PING`/`PONG`: liveness; more than 4 pings a second, an unknown type or a
  length over the bound closes the link.
- A second client while one is connected is accepted and closed at once;
  `BUSY` is best-effort (one non-blocking send attempt), so it may see only
  the close. It never displaces the first.
- Reconnecting starts a new local candidate from offset 0. Type numbers for
  TTY and debugger traffic are reserved; reserving them establishes no
  security or scheduling design for those channels.

### Viewer

- **States kept separate:** link state (connecting, streaming, ended,
  failed with reason), transport completion (END received and its size
  matches the bytes received), the reader's capture verdict, and each
  lane's measurement validity. END alone proves none of the last three.
- **Bounds:** at most 256 MiB accumulated per session (then the link is
  closed and labelled); one background parse job at a time, coalesced to
  the newest snapshot; snapshots are immutable copies of a received prefix;
  the UI thread never parses. A Disconnect button cancels the link and any
  pending parse.
- **Local file:** bytes are appended to `<name>.partial` as received. It is
  published as `<name>.prof` (exclusive create, never overwriting) only if
  the received length equals END's final size, the END result and flags
  show a successful shutdown, the reader says COMPLETE, and the local flush
  and close succeed. Otherwise it is kept as `<name>.incomplete.prof` with
  the reason. A local write error stops writing, keeps the in-memory copy,
  and is shown. The link's own failure is reported locally by the viewer,
  never only over the broken link.
- **Partial data:** every strict prefix of a VALID capture must load as
  INCOMPLETE (waiting for data); a test cuts a valid capture at every byte
  to prove it with the current reader. This is a check on valid captures,
  not a licence to relabel bad bytes: corrupt-prefix controls (a wrong
  constrained header byte, a bad chunk type) must stay MALFORMED.
- **Live label:** the view stays LIVE / PROVISIONAL until END is accepted,
  even if the reader already sees a complete trailer; the reader's verdict
  is shown beside it, separately.
- **Schema:** the frame types, payload layouts, string bounds and END
  fields are pinned in `sdk/libps3dkprof/src/live/live_proto.h`, the one
  schema the agent, its tests and the viewer use.
- **What is shown with the current reader:** until the run ends the capture
  is not COMPLETE, so the reader marks every lane not MEASUREMENT VALID and
  computes no zone statistics. The live timeline shows provisional,
  hatched spans, and Zones stays empty until the final COMPLETE capture.
  The reader is not loosened; live aggregates would need their own reviewed
  semantics.
- Follow keeps the window on the newest data; any zoom or pan turns it off.

### Tests and gates

- Host (loopback): late join from offset 0; a file grown by another thread;
  temporary EOF waits; END only after the finalization record, with the
  recorded size; start before init, after shutdown, twice, and with a
  callback sink; handshake timeout; oversized and unknown frames; ping
  flood; second client gets BUSY; stalled viewer (drain timing measured
  against no agent); stop deadline expiry without freeing state.
- Viewer: recorded streams split and merged at every frame boundary; gaps,
  overlaps and offset overflow; a prefix before the header, mid-chunk, and
  the trailer received before END; END with a failed shutdown but a
  parseable trailer; disconnect after the trailer; size mismatch at END;
  local write failure.
- Hardware gate 0, before any console-side viewer work: a probe proves an
  app thread can read its own capture file, while the sink's descriptor is
  open, on the HEN HDD (USB only if USB is offered), and sees bytes equal to
  the final file. RPCS3 or host success is not LV2 evidence.
- Hardware: matched runs with no agent, an idle agent, a connected viewer
  and a stalled viewer, each against the idle baseline; frame-time and
  drain cost reported.

**Not in phase 2:** TTY, debugger traffic, several viewers, resume,
starting or stopping a capture from the PC (P16), live aggregates, and any
change to the capture format.

### Target integration (design for review)

Gate 0 is closed for its narrow scope (2026-11-01): on HEN 3.5.0 / 4.93, an
app thread reads the profiler's open FILE-sink capture on the internal HDD,
byte-exact, through newlib/librt (`samples/profiling/live-read-probe`). This
section turns the host agent into a console agent, in three checkpoints,
each reviewed before the next: (1) the runtime record and public API;
(2) the `live_os` extraction, with the existing host, fault and real-agent
end-to-end tests rerun unchanged; (3) the LV2 layer, the sample wiring and
the hardware gates. Nothing here is claimed until its gate has a result.

**Capture record (checkpoint 1).**
- Type `ps3dk_prof_capture_record`, read by
  `int ps3dk_prof_get_capture_record(ps3dk_prof_capture_record *out)`.
  Fields: `char path[256]` (NUL-terminated, at most 255 bytes, exactly the
  path the file sink created), `uint64_t session_id` (the init timebase
  value), `char app[33]` and `uint32_t app_len`, `uint8_t build_id[512]` and
  `uint32_t build_id_len` (the configured limits). The call copies into the
  caller's struct; the caller owns its copy.
- A successful `ps3dk_prof_init()` with the FILE sink fills the runtime's
  record and publishes it with a release store as its last step. Readers
  load the flag with acquire. A failed init publishes nothing.
- Results: `PS3DK_PROF_OK`; `PS3DK_PROF_ENOTINIT` before a successful init;
  `PS3DK_PROF_ENOTSUP` (new, -17) with a callback sink. The record stays
  readable, unchanged, after shutdown, for the runtime's lifetime.

**Public live API (checkpoint 1, implemented in checkpoint 3).**
- `int ps3dk_prof_live_start(const ps3dk_prof_live_config *)`,
  `int ps3dk_prof_live_stop(uint64_t graceful_us, uint64_t join_us)`, in
  `ps3dk/prof.h`, compiled out (returning `PS3DK_PROF_ENOTSUP`) when the
  profiler is.
- Start reads the capture record and the finalization accessor itself; the
  app passes no path. It refuses: before init (`ENOTINIT`); once shutdown
  has begun (`ESTOPPED`); a callback sink or a capture path outside
  `/dev_hdd0/` (`ENOTSUP`: HDD only until another device is measured); a
  second start (`EALREADY`); no socket or listener (`ENET`, new, -18).
- Stop returns `OK`; `ETIMEDOUT` (new, -19) or `ECLOCK`: still stopping,
  call stop again; `ETHREAD`: the join failed, terminal, never retried; or
  `ENOTINIT` if the agent was never started.
- The agent object is static storage inside `libps3dkprof`, never freed.
  Stop's deadlines bound its waits; they do not bound an LV2 file call or
  the ordinary join after the thread's last store.
- Calls stay caller-serialized.

**`live_os` contract (checkpoint 2).** The protocol and state machine stay
in `live_agent.c`; every OS call goes through:
- `listen(port, loopback) -> fd`, `accept(lfd) -> fd | AGAIN`, both
  non-blocking; `send(fd, buf, n)` and `recv(fd, buf, n)` non-blocking,
  returning bytes or a normalized code: `AGAIN`, `INTR` (retried, at most 16
  times in a row), `CLOSED` (peer gone), `ERROR`; `poll(fds, timeout_ms)`;
  `close(fd)`, called exactly once by the owner of the descriptor.
- `send` never raises a signal: the host uses `MSG_NOSIGNAL`. Whether a
  send on a closed LV2 socket can raise a signal is UNVERIFIED; the shim
  treats `EPIPE` as `CLOSED`, and the checkpoint 3 smoke checks a send after
  the viewer has gone (the app keeps running, the agent sees `CLOSED`).
- `file_open(path) -> fd` read-only; `file_read_at(fd, buf, n, off)` as
  `lseek` + `read` on a descriptor owned only by the agent thread (no other
  thread touches its offset).
- Seam tests: the host shim's normalization under injected `EAGAIN`,
  `EINTR`, `EPIPE` and a short send; then the 28 H1 fault cases and the
  real-agent end-to-end test, unchanged, against the extracted code.

**Budget (checkpoint 3).**
- Static, exact and reported: the agent object (`sizeof`, about 66 KiB,
  mostly one frame buffer), the capture record (about 0.8 KiB), and the
  strings, taken from the link map of the sample. The agent uses no TLS of
  its own; any TLS pulled in from the network layer is listed. Budget: at
  most 72 KiB; the observed figure and its margin are reported for both
  ABIs.
- Stack: 32 KiB is a CANDIDATE, not accepted. A measurement build reads
  the thread's stack bounds from `sys_ppu_thread_get_stack_information` at
  thread start and refuses to paint unless its current stack pointer lies
  inside them. It paints from 1 KiB below the stack pointer down to the
  lowest address, and on exit scans up from the lowest address for the
  first overwritten word. The reported high-water mark is the TOTAL depth
  from the top of the stack to that word, so the unpainted top region (the
  entry frames and the 1 KiB gap) counts as used. The painting code is in
  the checkpoint 3 source review. The high-water mark and its margin are
  reported for both ABIs on the real SDK call path (socket, poll, file
  read). The candidate is accepted only if the high-water mark is at most
  half the stack; the link map alone proves reservations, not use.

**Linkage and off behaviour.**
- The agent, the shim and everything they call are built uninstrumented, as
  the rest of the runtime.
- They are separate archive members, pulled in only by a reference to
  `ps3dk_prof_live_start`. A profiling-off build or a build that never calls
  it links no live object, opens no listener and has no network dependency;
  a link-map test checks both.
- Live support is in the manual-lifecycle archive only, not the automatic
  one. The archive-member gate and the package inventories list the new
  members.

**Sample (checkpoint 3).** `heterogeneous-workload` gains `SAMPLE_LIVE`,
which requires `SAMPLE_PROFILING` (a configure error otherwise), on in the
hardware package.
- `netInitialize`, then `ps3dk_prof_live_start` after profiler init. A start
  failure is printed and shown on screen; the capture continues and stays
  valid.
- The screen shows the console's IP and the port for Connect.
- Teardown branches on the stop result: `OK` -> `netDeinitialize`;
  `ETIMEDOUT`/`ECLOCK` -> stop once more; still not `OK` -> leave the
  network initialized (process exit releases it) and print that the agent
  did not finish; `ETHREAD` -> no `netDeinitialize`.
- The README shows the calls an app adds.

**Target gates (checkpoint 3)**, each with its own evidence:
1. Build: both ABIs, `-Werror`; the archive check still refuses test-hook
   symbols; the off/unused link-map test; the static budget.
2. HEN smoke: the sample with the agent, the PC app connected from the
   start: a published `.prof` byte-equal to the file left on the console,
   reader COMPLETE, stop `OK`. A second run joins mid-run. Stack
   high-water reported.
3. Overhead: matched HEN runs of the same sample build: no agent, agent
   idle (no viewer), viewer connected, viewer stalled (connects then stops
   reading). Reported per run: frame time p50/p90/p99, drain time, capture
   verdict and lane validity, with a matched no-input idle baseline per set.
   Numbers are reported, not turned into a pass mark, until the director
   sets one.
4. Failure paths on the console: a viewer drops mid-run (the app keeps
   running; the next viewer starts from offset 0); the agent is stopped
   while a viewer is stalled: stop's waits are bounded and no END is sent;
   the file and join limits above still apply.

**Not in this design:** USB or other devices, several viewers, TTY or
debugger channels, resume, the automatic lifecycle, and any change to the
capture format.

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

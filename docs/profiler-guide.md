# Profiler user guide

The PS3DK profiler (`libps3dkprof`) records a PPU zone capture into a `.prof` file, and can optionally add SPU-lane and RSX-domain zones. On the PC side, `ps3dk-prof check` and `ps3dk-prof report` read a capture from the command line, and the `ps3dk-prof-viewer` GUI displays one.

The `PS3DK_PROF` interface in `<ps3dk/prof.h>` is PPU-only: an SPU translation unit that has `PS3DK_PROF` set fails to build.  SPU-lane and RSX-domain zones come from a separate, EXPERIMENTAL extension (section 2.8).

---

## 1. What the profiler is

| Piece | Role |
|---|---|
| `libps3dkprof.a` | PPU capture runtime; writes the `.prof` file (format v1); optional SPU-lane / RSX-domain extension. |
| `ps3dk-prof` | Command-line checker (`check`) and reporter (`report`) for a captured file. |
| `ps3dk-prof-viewer` | GUI viewer: overview, timeline, frames, zones, hot paths, live sessions, and full-frame snapshots. |

A run produces one capture. A capture is read back, validated, and displayed. The viewer's "Capture full frame" feature takes a snapshot of one frame of the capture. "Capture RSX frame" is PLANNED (section 5).

---

## 2. PS3 side, step by step

### 2.1 Include the header and enable it

In every C / C++ translation unit that contains zones:

```c
#include <ps3dk/prof.h>
```

Compile that translation unit with `-DPS3DK_PROF=1`. Without the flag (or with it 0), every `PS3DK_PROF_*` macro is inert: its arguments are not evaluated, no `libps3dkprof` function is referenced, and a zone guard returns a typed no-op handle. The public types and result codes are declared in both builds, so source can keep its zone calls regardless of the flag.

### 2.2 Link the profiler

The profiler archive is installed for both ABIs:

```
ILP32 : -L${PS3DK}/ppu/lib       -lps3dkprof
LP64  : -L${PS3DK}/ppu/lib/lp64  -lps3dkprof
```

Link the ordinary PS3DK runtime libraries your app already uses alongside it (the sample links `dbgfont gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m`).

### 2.3 Choose a mode

`ps3dk_prof_config.mode` is `PS3DK_PROF_MODE_APP` (`1`) or `PS3DK_PROF_MODE_THREAD` (`2`).

| | APP | THREAD |
|---|---|---|
| Who may record zones | Any PPU thread registered with `ps3dk_prof_thread_begin`. With `auto_register` set, a thread is registered on its first zone; otherwise zones from unregistered threads are ignored. | Same. |
| Background drain | None: the app calls `PS3DK_PROF_DRAIN` itself. | An internal thread drains every `drain_interval_us`. |
| Who may drain | Only the designated drain thread; from any other thread a drain returns `PS3DK_PROF_EPERM`. | The internal drain thread. |

`ps3dk_prof_config` fields and their ranges (see `<ps3dk/prof.h>` for the complete list and error codes): `mode`, `max_threads` (1..1024), `max_descriptors` (1..65536, one per zone site), `ring_records` (power of two, 512..1048576), `drain_interval_us` (1000..1000000, THREAD mode), `drain_stack_bytes` (16 KiB..1 MiB, THREAD mode), `quiesce_timeout_us` (0..60000000), `auto_register` (`0` default / `1`), `app` (1..32 characters of `[A-Za-z0-9_-]`, names the default capture file), `build_id`, `flags`, `workload_tag`, and a `reserved` that must stay `0`.

`ps3dk_prof_config_defaults(&cfg)` starts `cfg` from a valid default start (see `<ps3dk/prof.h>` for the exact values). Set `cfg.mode` and `cfg.app` explicitly before calling `ps3dk_prof_init`.

### 2.4 Init, read the capture record, drain, shutdown

Before `ps3dk_prof_init` you may name the sink location or supply a callback sink:

- `ps3dk_prof_set_sink_path(dir)` — a DIRECTORY (1..255 bytes). The file is `<dir>/<app>-<16 hex digits of the init timebase>.prof`, created exclusively; an existing file is `PS3DK_PROF_EEXIST`.
- `ps3dk_prof_set_sink_callbacks(&cbs)` — a caller-supplied `{ write, flush, close, ctx }` instead of the file sink. `ps3dk_prof_get_capture_record` and the live view are `ENOTSUP` with a callback sink.

```c
ps3dk_prof_config cfg;
ps3dk_prof_config_defaults(&cfg);
cfg.mode = PS3DK_PROF_MODE_APP;      /* or _THREAD */
cfg.app  = "my-app";                 /* 1..32 [A-Za-z0-9_-] */
if (ps3dk_prof_init(&cfg) == PS3DK_PROF_OK) {
    ps3dk_prof_capture_record cap;
    if (ps3dk_prof_get_capture_record(&cap) == PS3DK_PROF_OK) {
        /* cap.path, cap.session_id, cap.app, cap.build_id (build_id_len bytes) */
    }
} else {
    /* the app runs without a capture; every zone call stays harmless */
}
```

`ps3dk_prof_get_capture_record` is `OK` after a successful init, `ENOTINIT` before one (or after a failed init), and `ENOTSUP` with a callback sink (no file). The record is published once and immutable for the runtime's lifetime.

In APP mode, call `PS3DK_PROF_DRAIN` regularly (for example once per frame) from the designated drain thread; the sample drains after each frame. In THREAD mode an internal thread drains for you.

At the end, `ps3dk_prof_shutdown` waits up to `quiesce_timeout_us` for the threads still recording to quiesce, drains what remains, writes the trailer (its flags record a run that did not quiesce), then calls `flush` and `close`, and returns the run's result. The meaningful codes are `PS3DK_PROF_OK` (clean final), `ENOTQUIESCED`, `ESEQLIMIT`, `ESINKRECOVERED`, `ESINKFAIL`, `ECLOSE` (a `close()` failure after the trailer: the run FAILS), `ECLOCK`, and `ETHREAD`; see `<ps3dk/prof.h>` for the full set.

### 2.5 Zones

Each zone site declares a function-local `static` descriptor. Begin / end:

```c
ps3dk_prof_zone h = PS3DK_PROF_ZONE_BEGIN("compute");
/* ... work ... */
PS3DK_PROF_ZONE_END(h);
```

A scoped guard ends automatically at scope exit (C uses `__attribute__((cleanup(...)))`; C++ uses the RAII `ps3dk_prof::zone_guard`, which is neither copyable nor movable):

```c
PS3DK_PROF_ZONE("compute");
```

The `_FOR` variants tie a zone to an explicit frame token:

```c
ps3dk_prof_token tok = PS3DK_PROF_FRAME_TOKEN();
ps3dk_prof_zone h = PS3DK_PROF_ZONE_BEGIN_FOR("frame-work", tok);
/* ... */
PS3DK_PROF_ZONE_END(h);
```

A zone handle `kind` is `0 INERT`, `1 PUSHED`, `2 REFUSED`, or `3 SPENT`; a zero-initialised zone is `INERT`. A descriptor `scope` is `1 cpu` or `2 gpu-complete`.

### 2.6 Frames and threads

- `PS3DK_PROF_FRAME()` advances to the next frame on the calling thread.
- `PS3DK_PROF_FRAME_TOKEN()` returns the current frame token (`{owner, reserved, frame}`); `PS3DK_PROF_TOKEN_NONE` is `{0,0,0}` (owner `NONE`). Tie a zone to a frame with `_FOR`.
- Register each PPU thread that records zones with `ps3dk_prof_thread_begin(name)` before its first zone (or set `auto_register`); `ps3dk_prof_thread_end` releases it.

`ps3dk_prof_init`, `ps3dk_prof_live_start`, `ps3dk_prof_shutdown` and `ps3dk_prof_live_stop` are caller-serialized: the app must never call two of them at the same time (and `ps3dk_prof_live_get_stats` is serialized with `live_start` and `live_stop`).

### 2.7 Where the file goes and the live view

The default sink writes `<dir>/<app>-<session_id>.prof`, where `session_id` is the timebase value at init. The sample writes under `/dev_hdd0/tmp/<app>-<session>.prof`. The live view (below) serves only captures on `/dev_hdd0/`.

**Live view.** The live agent serves this run's FILE-sink capture to one PC viewer at a time over TCP while it grows; it only reads the file, so the writer never waits for a viewer. The network must be initialised first.

Link `net` and `netctl_stub` in addition to the profiler. After a successful `ps3dk_prof_init`:

```c
int live = 0;
if (netInitialize() == 0) {
    if (ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK)   /* NULL config: every default */
        live = 1;
    else
        netDeinitialize();                             /* no agent thread exists */
}
```

`ps3dk_prof_live_config` documented defaults (see `<ps3dk/prof.h>`): `port` `31750`, `poll_us` `10000`, `send_timeout_us` `2000000`, `handshake_timeout_us` `2000000`, `stack_bytes` `64 KiB`, `cap_bytes` up to `2^40`. A zero field takes its default; a NULL config takes every default; out-of-range values or a non-zero `reserved` field is `EINVAL`.

`ps3dk_prof_live_start` returns `OK` on success, or `ENOTINIT` (before init), `ENOTSUP` (a callback sink, or a capture not on `/dev_hdd0/` — the only device measured), `EALREADY` (a start already succeeded this run), `EINVAL`, `ENET` (listener could not be created), `ETHREAD` (agent thread could not be created), or `ESTOPPED` (shutdown already began).

**Shutdown-then-stop order.** `ps3dk_prof_shutdown` must complete before `ps3dk_prof_live_stop`, because the connected viewer needs the `END` finalization that the shutdown produces. The sample's `source/live.c` does it in this order:

1. After `ps3dk_prof_init`: `netInitialize`. If it fails, skip the live view.
2. `ps3dk_prof_live_start` (a zeroed config takes the defaults). If it fails, keep running and call `netDeinitialize` at once — no agent thread exists.
3. At the end: `ps3dk_prof_shutdown` first, so a connected viewer can get `END`.
4. `ps3dk_prof_live_stop(graceful_us, join_us)`. On `ETIMEDOUT` or `ECLOCK` call it again. Only after it returns `OK`, call `netDeinitialize`; after `ETHREAD`, or a stop that never returns `OK`, leave the network initialized.

`ps3dk_prof_live_stop` returns `OK` (stopped), `ETIMEDOUT` / `ECLOCK` (still stopping: call stop again, keep the network initialized), `ETHREAD` (the join failed: terminal, never retried, and the network must NOT be torn down), `EALREADY`, or `ENOTINIT`. A `graceful_us` of 0 cancels at once. The deadlines do NOT bound an LV2 file call already in progress, nor the final ordinary join once the thread has left its loop (no API bounds that wait).

`ps3dk_prof_live_get_stats(&stats)` (EXPERIMENTAL) copies the agent's final counters once a `live_stop` has succeeded; each admitted, finished connection ends in exactly one `drop_*` field.

Only captures on the internal HDD (`/dev_hdd0/`) are served. The live view has been run on a HEN console with the heterogeneous-workload sample, and the viewer's saved copy matched the console's file byte for byte; its overhead has not been measured, and the agent still uses CPU, memory and I/O while it serves.

### 2.8 SPU lanes and RSX domains (EXPERIMENTAL)

This records:

- each SPU worker's jobs as `spu.work` zones on its own SPU lane, and
- the RSX scene's command blocks as `rsx.setup` and `rsx.draw` zones on an RSX domain,

so the viewer's Timeline shows PPU, SPU and RSX rows.

**Build it from the extension archive.** The SPU-lane / RSX-domain extension is not part of the installed SDK; it is the **uninstalled extension archive**, built from the SDK's `sdk/libps3dkprof` tree:

```
make -C sdk/libps3dkprof ext-ilp32 ext-live-ilp32   # ILP32: libps3dkprof_ext.a and libps3dkprof_ext_live.a
make -C sdk/libps3dkprof ext-lp64 ext-live-lp64     # LP64 (-mlp64): the same members, in their own directories
```

In the sample's `CMakeLists.txt`, turn it on with `-DSAMPLE_PROF_EXT=ON`, pointing:

- `SAMPLE_EXT_ARCHIVE` at the uninstalled `libps3dkprof_ext.a`, and
- `SAMPLE_EXT_LIVE_ARCHIVE` (only with `SAMPLE_LIVE`) at the uninstalled `libps3dkprof_ext_live.a`.

Both archives must be the data model the sample is built for (`SAMPLE_ABI`, with the matching toolchain file); the sample checks every member's data-model tag at configure time.  The SPU program takes some PPU addresses as 32-bit values in both data models, which relies on every LV2 process address being below 4 GiB; the PPU side checks each one and refuses to pack a larger address (the worker then runs unprofiled, SPU profiling is reported off and `w32_eligible` is cleared).  `SAMPLE_PROF_EXT` requires `SAMPLE_PROFILING`; it links the extension archive (and the live archive, if live) instead of the SDK's `libps3dkprof.a`. An app that does not opt in is unchanged.  The extension has no installed public header yet; its use is documented by the sample: `samples/profiling/heterogeneous-workload/README.md`, its `CMakeLists.txt` and `source/prof_ext.h`, and the extension sources under `sdk/libps3dkprof/src`.

**The W32 premise.** SPU times come from each SPU's 32-bit decrementer.  W32 ("assume no decrementer wrap") is the premise that no worker's decrementer wrapped within an epoch.  It is not something the profiler guarantees: the app has to make it true and show it.  The sample does so with its own protocol: it resets a worker's decrementer on the first job word, when a reset is due (its dispatch threshold is 20 s, with acknowledgements, deadlines and a service bound under 22 s) or on a reset-only word, and any failed step permanently clears eligibility.  W32 is justified for a capture only when the sample's completed `.ext-end.txt` receipt for that run reports `w32_eligible=1` and its lifecycle checks passed.  Even then, W32 does not guarantee that every SPU zone can be placed: an epoch with too few clock observations, or a reader budget refusal, still leaves part of a lane unplaced.  Without W32 (the R64 premise), SPU lane rows show as UNMAPPABLE.

GPU-side, the sample uses reports 32..63 and label 241 for the domain, and label 242 for the application fence. These values are part of the sample's build, not a public contract; they may change before release.

#### Cost and the drain

Measured on a console with the heterogeneous-workload sample (ILP32): one PPU zone pair costs about 0.5 us; PPU profiling adds about 10 to 50 us per frame, and PPU plus SPU and RSX about 60 to 90 us.  One SPU zone pair costs about 21 decrementer ticks on the SPU.  The per-frame drain is the largest cost: about 0.2 ms per frame, and up to 1.6 ms per frame in phases with heavy SPU zone traffic.  The extension runs in APP mode, so the drain runs on the designated drain thread; to keep it off your render thread, make a separate PPU thread the designated drain thread and call `PS3DK_PROF_DRAIN` from it.

#### SPU zone timing (decrementer ticks, not wall time)

SPU zone durations are shown as **SPU decrementer ticks**, never as wall time. The viewer does not convert them to microseconds or seconds.

Each SPU zone is placed on the timeline only inside the **PPU-side drain and ping** timestamps that bound it. The viewer draws those bounds; the SPU row does not extend past them.

Tighter bounds come from **draining and pinging more often**: every drain and every ping adds a fresh PPU-side timestamp that the SPU row can sit inside.

This placement does not use one straight-line fit per epoch (the clock map the W32 setting below selects). Under load the SPU decrementer does not run at a steady rate against the PPU clock, and a single fit per epoch is not a fair model of what the hardware was doing.

One possible explanation, **not yet proven**: while LV2 has the SPU thread switched out, the decrementer may stop. The ticks would then be the time the SPU actually ran, which is what matters for optimisation; placement still comes from the PPU-side bounds.


#### RSX zone states

RSX timing is judged per RSX unit (one GPU domain and epoch), and every zone in a unit shares its state. A unit is in one of three states: `MEASURED`, `CONDITIONAL` or `UNMEASURED`.

| State | What it means for you |
|---|---|
| `MEASURED` | The unit's zones are placed on the timeline within bounds the checks proved. On the console this was tested on, a placement is about **29 microseconds** wide; positions inside that width are not resolved. |
| `CONDITIONAL` | The zones are placed, but at least one requirement for `MEASURED` is not met. The label names the reason, for example a hardware profile that has not been reviewed. |
| `UNMEASURED` | The zones are in the capture but no placement could be proved for the unit. The label names the reason. |

A unit reaches `MEASURED` only when all of these hold:

- the capture's hardware profile is one the reader lists as reviewed;
- a held-out bracket check: every other **interior PPU bracket** of the unit is held back, one straight-line clock map is fitted from the rest, and each held-out bracket lands inside the prediction the fit makes for it, with **at least one** bracket decisive and **none** contradicted;
- at least **99 percent** of the capture's GPU begin/end pairs lie between the unit's first and last brackets (interpolated, not extrapolated).

The viewer never claims more accuracy than these checks prove. A `MEASURED` span is a placement, not a measurement of the draw time.


#### 32-bit and 64-bit builds

The SPU-lane and RSX-domain extension now builds for **both 32-bit and 64-bit PPU apps**. Two archives are installed: one for each data model.

The heterogeneous-workload sample checks at configure time that the archive's data model matches the app's (samples/profiling/prof-ext-abi.cmake); an app can reuse that check. A mismatch is a configure-time error, not a link-time or runtime surprise.

**Status:** EXPERIMENTAL.  Console runs of the heterogeneous-workload sample, one ILP32 and one LP64, placed every SPU zone pair and every GPU pair; RSX units read `MEASURED` under the reviewed RSX-R1 profile; SPU lanes are valid and placed but read `TICKS-CONDITIONAL` until an SPU profile is reviewed.  SPU zones inside SPURS jobs have not been tested.
---

## 3. PC side

### 3.1 Command line

```
ps3dk-prof check  capture.prof    # validate; report the verdict
ps3dk-prof report capture.prof    # print a human-readable report
```

Run these before interpreting a capture.

Exit status: `0` COMPLETE, `1` INCOMPLETE, `2` MALFORMED, `3` usage or I/O error, `4` COMPLETE but a domain-extension source is not clean.  A capture that ends without a trailer (the app quit from the XMB, the link ended, a copy cut short) is COMPLETE and exits `0`; its output has the extra line `ending: NO-TRAILER` (a file with a trailer has no `ending:` line), so a script that must tell the two apart greps for that line.

### 3.2 The viewer

The GUI viewer is started with an optional capture and flags (from the viewer's `main.rs`):

```
ps3dk-prof-viewer [capture.prof]
    [--page=overview|timeline|frames|zones|paths]   # default overview
    [--view=START:LEN]                              # timeline window (ticks from capture start)
    [--graph=bars|lines|distribution|map]           # frames page graph mode (default bars)
    [--connect=IP:PORT]                             # start a live session immediately
    [--capture-dir=DIR]                             # default save folder for live sessions
    [--min-size]                                    # window 860x540 (default 1240x780)
    [-v|--version]
```

Open a capture either by passing the file on the command line, or from inside the app with **Open**. For a live session, use the **Connect** form and enter the PS3's IP and port (default `31750`) plus a save folder.

### 3.3 Pages

- **Overview** — identity, clock, lanes, verdict; lists the extension sources when the capture carries SPU lanes or RSX domains.
- **Timeline** — PPU threads, SPU lanes and RSX domains as separate rows. SPU rows are interpreted under the W32 or R64 premise that is set for the capture ("SPU: assume no decrementer wrap (W32)"); bars whose timing was extrapolated are flagged. On SPU lanes and RSX domain tracks, shaded stretches labelled **no placed pairs** mark intervals where no placed completed pair's outer extent (earliest begin to latest end) meets that timeframe. This does *not* mean the SPU or RSX was idle: work that was not placed (such as unplaced pairs, censored spans, unpaired events, open spans, loss records, or epochs without an inventory whose events are unknown) may still fall in those stretches.
- **Frames** — per-frame bars measured against a 30 / 60 FPS budget, over-budget frames in red; per-lane tables. The **Graph** selector offers four display modes (also selectable with `--graph`):
  - **Bars** (default): PPU frame duration graph with budget threshold line, and SPU/RSX foreign lane strips below sharing the same slot columns and horizontal scroll.
  - **Lines**: Plots all series (PPU and lanes) on one shared tick axis, with frame k at the same horizontal position for every series. The PPU point is the exact elapsed time. A lane point is drawn at the upper bound (hi) of its conservative included-work bound, with lo..hi rendered as a translucent band (an infinite hi has no line point; its band runs to the top with an open-arrow cap). The line breaks at any slot without a value (unknown, refused, nothing placed, no interval); gaps are never bridged and never drawn as 0. Dots on the baseline mark measured frames with nothing included on that lane.
    - **Smoothing**: Segmented control (**Raw**, **Mean 5**, **Mean 15**, **Mean 30**) applying a trailing moving average (capped at 64 frames) within unbroken runs (fewer frames at run starts, never across gaps); raw lines remain visible faintly.
    - **Scale**: Shared with Distribution (**Fit p99** [default] or **Max**). **Fit p99** sets the top of the axis to 1.1 times the largest of the PPU p99, each lane's p99 upper bound and the budget; a series with fewer than 20 usable frames contributes its maximum instead of a p99. Larger values, +inf upper bounds and bounds not representable as finite values are drawn at the top edge with an upward arrow. If a lane's distribution was refused, only the Fit p99 axis uses that lane's finite maximum instead, with a warning note; the refused distribution itself stays shown as unavailable. **Max** scales so every finite representable value fits; +inf upper bounds and non-representable finite bounds are drawn at the edge with an arrow.
  - **Distribution**: Shows the distribution over measured frames per series (excluded frames are counted, never binned). PPU series shows an exact integer histogram and percentiles (p50, p90, p99; ZoneStats rank rule, refused below 20 measured frames). A lane series gives, per percentile, the enclosing interval [p-th smallest lo, p-th smallest hi] across frames that have a bar (order statistics are monotone). This bounds only retained DEFINITE start-attributed top-level included work on frames with a bar (ambiguous, extrapolated, exclusive-unavailable pairs, and frames with nothing included are omitted; bounds only included work, not total lane activity; refused below 20 frames with a bar).
  - **Verdict map**: Dense matrix with one row per series and one cell per frame, colored by budget verdict or slot state: `OVER`, `WITHIN`, `UNDETERMINED` (striped), `no verdict` (no budget or clock), `verdict unavailable` (exact comparison refused), `nothing included` (dot), `unknown` (outline), `refused` (crossed), or `no interval` (empty cell; no measured interval on lane).
  - **Stated limits**: Series are never summed: lanes run concurrently with the PPU and with each other, and lane values represent start-attributed, unclipped included work, not occupancy (neither stacked totals nor per-frame utilization ratios are established by the reader). Percentiles cover only frames with bars. Edge arrows mark +inf, non-representable or out-of-scale values. A refused lane distribution is shown as unavailable; only the Fit p99 axis falls back to that lane's finite maximum, with a warning.
- **Zones** — named-zone statistics for PPU zones only, from PPU lanes that are MEASUREMENT VALID (`ps3dk-prof report` prints each lane's validity; spans on other lanes are excluded); SPU and RSX zones are not listed. Columns: Zone (name and tag), Instances, Completed, Censored, Total, Mean, p50, p90, p99 and Max (all durations inclusive), and Qualification. Percentiles use the usable completed duration samples and are refused when there are fewer than 20 of them. Unknown values sort last.
- **Hot paths** — the call paths that account for the most measured time, for PPU threads and (when present) SPU lanes and RSX domains.

### 3.4 Live sessions

While data is streaming, the app bar shows a `LIVE · PROVISIONAL` badge and the progress strip reports `PROVISIONAL: still receiving (<bytes> so far); <coverage>`; the verdict is provisional until the session ends. The live link is served by the profiler's own agent thread. The agent drops the viewer as stalled when it has output pending and a send makes no progress for longer than `send_timeout_us` (default 2 s); this is not a deadline from the moment the viewer stops reading, because buffers may keep accepting bytes, and a link with nothing to send is never stalled. A closed peer or a failed send is recorded when the agent detects it. The console's capture file is written independently of the link, subject to its own writer succeeding; after the live link is stopped the app can read the drop counters with `ps3dk_prof_live_get_stats`.

When the link ends (the app's profiler shuts down, the link disconnects, or you press **Disconnect**), the outcome is classified as follows:

- if the console sent `END` and every check passed, the capture is saved as a normal `.prof` with the verdict `COMPLETE`;
- if the session ended without a trailer (for example **Disconnect**, a dropped link, or the app quit from the XMB so the console's `END` says no trailer was written) and what arrived is a valid prefix with at least one event chunk, the viewer saves that prefix as a normal `.prof` (a torn last chunk is cut) and shows `COMPLETE` with the note `ended without a trailer (app quit or the link ended)`. `ps3dk-prof check` reads the same file the same way;
- otherwise, including a protocol, integrity or local-publication failure even after some valid data arrived, the session is a failure: the viewer keeps what it could as `<name>.incomplete.prof` with the reasons, or reports that nothing could be saved. A `.incomplete.prof` file is never promoted to a complete capture when it is reopened.

---

## 4. Capture full frame (available now)

The **Capture full frame** button on the app bar:

1. Freezes the frame selected on the Frames page; if none is selected, the newest measured frame is used, preferring one whose SPU and RSX data has already arrived.
2. The result is a **SNAPSHOT** of the data observed so far, not a complete frame; later data can add to it.
3. The Snapshot page has one row per PPU thread, SPU lane and RSX domain:
   - solid core — surely ran, under the capture's timing premise (for SPU and RSX zones the bounds are conditional on the clock-mapping premises, and extrapolated bounds are predictions, not guarantees);
   - whiskers — timing uncertainty;
   - solid claimed spans;
   - faint outlined "possible" spans (unclaimed, may overlap);
   - hatched extrapolated spans.
4. A per-lane coverage list shows `OBSERVED`, `PARTIAL`, `NO DATA`, or `UNMAPPABLE` with reasons.
5. **Re-resolve with W32/R64** makes a new snapshot under the other SPU premise; **Delete snapshot** drops it; **Back** returns to the Frames page.
6. Bookmarks are written next to the saved capture as `<name>.frames.txt` (a live session: when the session's file is saved; an opened file: at once). Reopening a bookmark re-reads the whole saved file; a refused or changed file is reported, never used silently.
7. Limits: 64 snapshots held at a time per session, and an accounted memory budget of 1 GiB shared by all sessions (an accounting limit, not a guarantee about the viewer's process memory).

---

## 5. Capture RSX frame (PLANNED)

A **Capture RSX frame** button next to **Capture full frame** is planned: it will time the supported draw calls of one frame on an RSX domain (safe-form array, index and inline draws and surface clears, from PSL1GHT `rsx*` and cell-style GCM callers), each only when it can be timed safely.  Other calls (the `Unsafe` forms, immediate mode, calls that do not fit the current command-buffer segment or are refused) are counted, not timed, and leave the capture PARTIAL, as can a per-frame draw cap or lost data.  It is a supported-draw timing, not a full frame acquisition.  It is planned to be opted into at build time by linking a hooked `librsx_prof.a` (with the extension archive) instead of `librsx.a`; that archive does not exist yet.

**Status: design approved, implementation not started.  Names, flags and libraries may change before release.**

---

## 6. Troubleshooting

### Verdict words

| Verdict | Meaning |
|---|---|
| `COMPLETE` | The reader found the file structurally complete (header to trailer, every chunk valid).  Whether the run itself finalized cleanly, including the file's close, is not in the bytes: it is the result of `ps3dk_prof_shutdown` on the console (and, for a live session, the agent's END status). |
| `COMPLETE (ended without a trailer ...)` | Valid chunks, no trailer, at least one event chunk: the app quit, the link ended or the copy was cut short.  Reported by `ps3dk-prof` and the viewer alike (exit `0`, extra line `ending: NO-TRAILER`).  Every analysis runs on it as on a trailered file, up to the last whole chunk; a torn last chunk is dropped and its bytes are named in the note, and loss that was not yet reported at the cut cannot be seen.  SPU zones are placed PROVISIONALLY (causal bounds alone, no tick counts, counted apart from the final coverage) and the coverage says the lane final record was not received, because there is no `ORDER_FINAL`. |
| `INCOMPLETE` | The trailer says the run did not finish cleanly (`NOT_QUIESCED`, `SEQ_LIMIT`, a sink error), or the file has no event chunk at all (a header with nothing captured).  A live session that failed a check (a protocol, integrity or local-copy failure) keeps its copy as `<name>.incomplete.prof`; a missing trailer alone never does. |
| `MALFORMED` | The capture broke a rule: a bad checksum, an out-of-range length, bad structure or inconsistent counts inside the bytes that are there.  Only the missing ending is forgiven; corruption in the prefix is not. |

### A capture that quit from the XMB

The file has no trailer, and that is expected: the app did not reach `ps3dk_prof_shutdown`.  `ps3dk-prof check` prints `COMPLETE (ended without a trailer ...)` and exits `0`; the viewer shows the `COMPLETE` badge with the same note and saves a normal `.prof`.  Per-source lines of a domain-extension file still say `STRUCTURE INCOMPLETE (no DOMAIN_FINAL; the file has no valid TRAILER)`, which is the source's own fact and does not change the file's verdict.

### A live view that never connects

- Both sides on the same subnet, and the PC can open a raw TCP connection to the PS3's IP on `31750` (or the port the agent was started with).
- The capture must be on `/dev_hdd0/`.
- The sample's screen shows the address to connect to, or why the live view is off.

### SPU rows UNMAPPABLE

Turn on the W32 premise only when the app shows that no SPU decrementer wrapped within an epoch: for the sample, its completed `.ext-end.txt` receipt with `w32_eligible=1` and passing lifecycle checks.  Leave the R64 premise for captures you cannot justify as W32, and accept that those SPU rows will not map.  Under W32, a lane can still be PARTIAL or partly unplaced when an epoch had too few clock observations.

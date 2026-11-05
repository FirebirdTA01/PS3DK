# Profiler user guide

The PS3DK profiler (`libps3dkprof`) records a PPU zone capture into a `.prof` file, and can optionally add SPU-lane and RSX-domain zones. On the PC side, `ps3dk-prof check` and `ps3dk-prof report` read a capture from the command line, and the `ps3dk-prof-viewer` GUI displays one.

The `PS3DK_PROF` interface in `<ps3dk/prof.h>` is PPU-only: an SPU translation unit that has `PS3DK_PROF` set fails to build.  SPU-lane and RSX-domain zones come from a separate, EXPERIMENTAL, ILP32-only extension (section 2.8).

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

### 2.8 SPU lanes and RSX domains (EXPERIMENTAL, ILP32 only)

This records:

- each SPU worker's jobs as `spu.work` zones on its own SPU lane, and
- the RSX scene's command blocks as `rsx.setup` and `rsx.draw` zones on an RSX domain,

so the viewer's Timeline shows PPU, SPU and RSX rows.

**Build it from the extension archive.** The SPU-lane / RSX-domain extension is not part of the installed SDK; it is the **uninstalled extension archive**, built from the SDK's `sdk/libps3dkprof` tree:

```
make -C sdk/libps3dkprof ext-ilp32       # libps3dkprof_ext.a (ILP32 only)
```

In the sample's `CMakeLists.txt`, turn it on with `-DSAMPLE_PROF_EXT=ON`, pointing:

- `SAMPLE_EXT_ARCHIVE` at the uninstalled `libps3dkprof_ext.a`, and
- `SAMPLE_EXT_LIVE_ARCHIVE` (only with `SAMPLE_LIVE`) at the live members compiled with the same extension flags.

`SAMPLE_PROF_EXT` requires `SAMPLE_PROFILING` and ILP32; it links the extension archive (and the live archive, if live) instead of the SDK's `libps3dkprof.a`. An app that does not opt in is unchanged.  The extension has no installed public header yet; its use is documented by the sample: `samples/profiling/heterogeneous-workload/README.md`, its `CMakeLists.txt` and `source/prof_ext.h`, and the extension sources under `sdk/libps3dkprof/src`.

**The W32 premise.** SPU times come from each SPU's 32-bit decrementer.  W32 ("assume no decrementer wrap") is the premise that no worker's decrementer wrapped within an epoch.  It is not something the profiler guarantees: the app has to make it true and show it.  The sample does so with its own protocol: it resets a worker's decrementer on the first job word, when a reset is due (its dispatch threshold is 20 s, with acknowledgements, deadlines and a service bound under 22 s) or on a reset-only word, and any failed step permanently clears eligibility.  W32 is justified for a capture only when the sample's completed `.ext-end.txt` receipt for that run reports `w32_eligible=1` and its lifecycle checks passed.  Even then, W32 does not guarantee that every SPU zone can be placed: an epoch with too few clock observations, or a reader budget refusal, still leaves part of a lane unplaced.  Without W32 (the R64 premise), SPU lane rows show as UNMAPPABLE.

GPU-side, the sample uses reports 32..63 and label 241 for the domain, and label 242 for the application fence. These values are part of the sample's build, not a public contract; they may change before release.

**Status:** EXPERIMENTAL.  One run of the heterogeneous-workload sample on a HEN console placed all six SPU lanes and the RSX domain on the timeline; broader validation is pending.

---

## 3. PC side

### 3.1 Command line

```
ps3dk-prof check  capture.prof    # validate; report the verdict
ps3dk-prof report capture.prof    # print a human-readable report
```

Run these before interpreting a capture.

### 3.2 The viewer

The GUI viewer is started with an optional capture and flags (from the viewer's `main.rs`):

```
ps3dk-prof-viewer [capture.prof]
    [--page=overview|timeline|frames|zones|paths]   # default overview
    [--view=START:LEN]                              # timeline window (ticks from capture start)
    [--connect=IP:PORT]                             # start a live session immediately
    [--capture-dir=DIR]                             # default save folder for live sessions
    [--min-size]                                    # window 860x540 (default 1240x780)
    [-v|--version]
```

Open a capture either by passing the file on the command line, or from inside the app with **Open**. For a live session, use the **Connect** form and enter the PS3's IP and port (default `31750`) plus a save folder.

### 3.3 Pages

- **Overview** — identity, clock, lanes, verdict; lists the extension sources when the capture carries SPU lanes or RSX domains.
- **Timeline** — PPU threads, SPU lanes and RSX domains as separate rows. SPU rows are interpreted under the W32 or R64 premise that is set for the capture ("SPU: assume no decrementer wrap (W32)"); bars whose timing was extrapolated are flagged.
- **Frames** — per-frame bars measured against a 30 / 60 FPS budget, over-budget frames in red; per-lane tables.
- **Zones** — zone-level detail.
- **Hot paths** — the call paths that account for the most measured time, for PPU threads and (when present) SPU lanes and RSX domains.

### 3.4 Live sessions

While data is streaming, the app bar shows a `LIVE · PROVISIONAL` badge; the verdict is provisional until the session ends.  When the session ends (the app's profiler shuts down, or you press **Disconnect**):

- if the console sent `END` and every check passed, the capture is saved as a normal `.prof` with the verdict `COMPLETE`;
- if the session ended early (for example **Disconnect**) and what arrived is an eligible valid prefix, the viewer saves that accepted prefix as a normal `.prof` and shows `COMPLETE (ended early)`.  This is the viewer's acceptance of a prefix: the strict reader (`ps3dk-prof check`) still reports such a file as `INCOMPLETE`, because it has no trailer;
- otherwise, including a protocol, integrity or local-publication failure even after some valid data arrived, the session is a failure: the viewer keeps what it could as `<name>.incomplete.prof` with the reasons, or reports that nothing could be saved.  A `.incomplete.prof` file is never promoted to a complete capture when it is reopened.

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
| `COMPLETE (ended early)` | Viewer only: the accepted valid prefix of a live session that ended early, saved as a normal `.prof`.  The strict reader (`ps3dk-prof check`) reports the same file as `INCOMPLETE` (no trailer). |
| `INCOMPLETE` | The reader found the capture incomplete (for example no trailer).  A live session's kept copy in that state is named `<name>.incomplete.prof`. |
| `MALFORMED` | The capture could not be parsed (truncated, bad checksum, out-of-range length, or a structural error). |

### A live view that never connects

- Both sides on the same subnet, and the PC can open a raw TCP connection to the PS3's IP on `31750` (or the port the agent was started with).
- The capture must be on `/dev_hdd0/`.
- The sample's screen shows the address to connect to, or why the live view is off.

### SPU rows UNMAPPABLE

Turn on the W32 premise only when the app shows that no SPU decrementer wrapped within an epoch: for the sample, its completed `.ext-end.txt` receipt with `w32_eligible=1` and passing lifecycle checks.  Leave the R64 premise for captures you cannot justify as W32, and accept that those SPU rows will not map.  Under W32, a lane can still be PARTIAL or partly unplaced when an epoch had too few clock observations.

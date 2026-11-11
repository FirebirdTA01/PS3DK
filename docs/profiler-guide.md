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
- Register each PPU thread that records zones with `ps3dk_prof_thread_begin(name)` before its first zone (or set `auto_register`); `ps3dk_prof_thread_end` releases it. The opt-in wait-observation variants also register the caller of successful profiler initialization as `init`; registering that thread again returns the expected `PS3DK_PROF_EALREADY`. Default archives do not add that automatic registration. For an existing worker that explicitly registers after initialization, establish that its runtime start hook has finished before initialization; creating the thread alone does not establish that ordering.
- The initial wait-observation adapter records timed mutex calls only after their initial trylock reports busy and the deadline is validated. Immediate-success timed locks and validation refusals are uncovered; absence of a timed-wait row is not a count of those calls or evidence of a dropped record.

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

**Link the installed extension archives.** The SDK installs `libps3dkprof_ext.a` and `libps3dkprof_ext_live.a` for each PPU ABI, in `ppu/lib` (ILP32) and `ppu/lib/lp64` (LP64). The extension headers `prof_spu.h`, `psp_desc.h` and `prof_gpu.h` are installed under `ppu/include/ps3dk`. The normal SDK build and install include these artifacts; the extension remains EXPERIMENTAL.

For `samples/profiling/heterogeneous-workload`, set `SAMPLE_PROFILING=ON` and `SAMPLE_PROF_EXT=ON`; add `SAMPLE_LIVE=ON` for live viewing. Use the toolchain file matching `SAMPLE_ABI`. By default, the sample selects the installed extension archive and, when live viewing is enabled, its matching live archive. `SAMPLE_PROF_SDK` selects an alternative SDK root with the same layout.

For development against build-tree archives, explicitly set `SAMPLE_EXT_ARCHIVE` and, with live viewing enabled, `SAMPLE_EXT_LIVE_ARCHIVE`. Do not mix an explicit extension archive with an implicitly selected live archive. `make -C sdk/libps3dkprof ext-release` builds the profile-enabled extension and live archives for both ABIs; the normal install places them in the SDK.

The sample still compiles the SPU writer from `SAMPLE_PROF_SRC`, which defaults to this repository's `sdk/libps3dkprof/src`. Installed PPU archives alone do not replace that source dependency. See the sample's README, CMakeLists.txt and `source/prof_ext.h` for the build and lifecycle example.

The sample checks every archive member's data-model tag at configure time. Its SPU program receives some PPU addresses as 32-bit values in both data models; the PPU side checks each address and refuses to pack a value above 4 GiB. On refusal the worker runs unprofiled, SPU profiling is reported off, and `w32_eligible` is cleared. With extensions enabled, link the extension archive and its optional live archive instead of `libps3dkprof.a`. An app that does not opt in is unchanged.

**Legacy captures and W32.** For captures without SPU order records, W32 ("assume no decrementer wrap") is an application-supplied premise that no worker's decrementer wrapped within an epoch. The legacy sample supports that premise with first-job, due-reset and reset-only words, acknowledgements and deadlines. Its 20 s dispatch threshold and service bound under 22 s are checked, and a failed step permanently clears eligibility. Use W32 only with the matching complete `.ext-end.txt` receipt reporting `w32_eligible=1` and successful lifecycle checks. W32 alone does not guarantee placement or overcome resource refusals.

Captures with SPU order records use the tick and causal-placement analysis described below. The W32 toggle does not establish the additional RMAX1 premise or turn SPU decrementer ticks into wall time.

GPU-side, the sample uses reports 32..63 and label 241 for the domain, and label 242 for the application fence. These values are part of the sample's build, not a public contract; they may change before release.

#### Cost and the drain

Measured on a console with the heterogeneous-workload sample (ILP32): one PPU zone pair costs about 0.5 us; PPU profiling adds about 10 to 50 us per frame, and PPU plus SPU and RSX about 60 to 90 us.  One SPU zone pair costs about 21 decrementer ticks on the SPU.  The per-frame drain is the largest cost: about 0.2 ms per frame, and up to 1.6 ms per frame in phases with heavy SPU zone traffic.  The extension runs in APP mode, so the drain runs on the designated drain thread; to keep it off your render thread, make a separate PPU thread the designated drain thread and call `PS3DK_PROF_DRAIN` from it.

#### SPU zone timing (decrementer ticks, not wall time)

SPU zone durations are shown as **SPU decrementer ticks**, never as wall time. The viewer does not convert them to microseconds or seconds.

For captures with SPU order records, the reader first bounds each recorded observation using identity-bound PPU evidence: lane creation, ordered calibration or ping brackets, and drain timestamps. The causal-only bounds remain available for inspection.

Finalized placement can tighten those bounds using **RMAX1**, an additional rate-ceiling assumption. Its default assumes the decrementer advances no faster than one decrementer tick per PPU timebase tick. The observed isolation rate does not establish this as an exact universal hardware ceiling. Placement and any solid core using the tightened endpoints are conditional on RMAX1 and the other declared premises; a contradiction leaves the affected endpoints unplaced.

Without `ORDER_FINAL`, supported prefix placement is **PROVISIONAL** and uses causal bounds alone. It contributes neither final tick counts nor final placement coverage. More frequent usable drains and brackets may tighten causal bounds, but cannot guarantee placement when identity, order or coverage is missing.

No single affine clock fit per epoch is used for this SPU placement. The measured workload captures contradicted that wall-clock model. The decrementer's behavior under descheduling remains an interpretation question: a tick count does not prove CPU execution time or the cause of a stall.

#### RSX zone states

RSX timing is judged per RSX unit (one GPU domain and epoch). A unit is `MEASURED`, `CONDITIONAL` or `UNMEASURED`; its state and reasons are distinct from the capture's coverage result.

| State | Meaning |
|---|---|
| `MEASURED` | The unit meets the reader's profile, raw-validity, clock-map, chronological-order, observation and hold-out checks. Its bounds remain conditional on the declared timing and producer premises. |
| `CONDITIONAL` | A measurement qualification is missing; inspect the listed reasons, such as a profile that is not reviewed or no decisive hold-out. Any displayed bounds retain their stated assumptions. |
| `UNMEASURED` | A required unit check failed, for example raw validity or a consistent bounded clock map. Inspect the reason and any separate placement output; this label alone is not a claim that every possible placement is unavailable. |

The reviewed RSX-R1 entry is scoped to its recorded runtime digests and configuration. This candidate requires `LOADER_HOOK=1`, `LIVE_LINKED=1` and the measured extension configuration. Hardware-model and system-software fields that the runtime cannot read remain explicitly unchecked; this is not evidence across other hardware or firmware.

The held-out bracket check, **H-SPLIT**, uses separate odd and even interior-bracket passes. In each pass, the kept brackets define a feasible affine map set. A held-out observation passes the compatibility check when its interval intersects the prediction, not only when the prediction contains the whole observation. A decisive check must be capable of rejecting an observation allowed by the surrounding evidence. These tests examine brackets; they do not independently establish event timing between them. Available identity-bound causal constraints are also checked separately against the bracket-only map. Qualification requires at least one decisive hold-out and no contradicted hold-out, alongside the other unit checks.

The **99 percent coverage policy is capture-wide**. Its denominator is retained eligible GPU pairs across the RSX units; its numerator is pairs in `MEASURED` units with both endpoints between that unit's first and last bracket coordinates. The policy requires a nonempty denominator, and omissions or unknown inventory are reported separately. Passing this policy does not confer `MEASURED` on any unit or prove that omitted work was absent.

Use the reported interval width for the particular capture and pair. A placement or zone duration bounds timestamped command intervals under the premises; it is not a direct measurement of shader cost, RSX busy time or utilization.

#### 32-bit and 64-bit builds

The SPU-lane and RSX-domain extension builds for **both ILP32 and LP64 PPU apps**. Each data model has its own `libps3dkprof_ext.a` and `libps3dkprof_ext_live.a`; select the pair matching the app ABI.

The heterogeneous-workload sample checks at configure time that the archive's data model matches the app's (samples/profiling/prof-ext-abi.cmake); an app can reuse that check. A mismatch is a configure-time error, not a link-time or runtime surprise.

**Status:** EXPERIMENTAL.  Console runs of the heterogeneous-workload sample, one ILP32 and one LP64, placed every SPU zone pair and every GPU pair; RSX units read `MEASURED` under the reviewed RSX-R1 profile; SPU lanes are valid and placed but read `TICKS-CONDITIONAL` until an SPU profile is reviewed.  SPU zones inside SPURS jobs have not been tested.
---

## 3. PC side

### 3.1 Command line

```
ps3dk-prof check  capture.prof    # validate; report the verdict
ps3dk-prof report capture.prof    # print a human-readable report
ps3dk-prof paths --functions [--elf app.elf] [--thread NAME] [--interval N | --all] [--top K] [--json] capture.prof
                                  # automatic function paths (captures from libps3dkprof_func.a)
```

Run these before interpreting a capture.

Instrument selectively with `-finstrument-functions`: omit tiny hot leaf files
with `-finstrument-functions-exclude-file-list`, or mark individual functions
`__attribute__((no_instrument_function))`. A fixed ILP32 console sample measured
about 0.43 microseconds of added elapsed time per instrumented entry in one
OFF/ON/OFF sequence. This is a descriptive workload comparison, not an isolated
hook-cost estimate; both self and inclusive times include hook cost. Keep the
same selection when comparing captures.

Start observation before entering the functions whose paths you need. If an
instrumented function enters before profiler initialization and exits after
registration, its unmatched exit increments the orphan counter without adding
a call path or elapsed time. An absent path does not mean the function took
zero wall time. The heterogeneous-workload sample offers a separate registration
probe for this case; it is outside that sample's exact call-tree check.

`paths --functions` prints, per thread, each caller chain with its calls, inclusive and self elapsed time and their share of the thread's interval, then the thread's counters (depth_limit, lost, unbalanced, orphan, deferred, reentry, clock) and statuses. Elapsed is wall time between the thread's hook events (waits, preemption and hook cost included), not CPU time. The default is the whole capture; `--interval N` shows one of the thread's own intervals and `--all` every chunk in order. A drain the thread did not answer (LAGGING) and an unacknowledged tail (INCOMPLETE_TAIL) are printed as UNKNOWN, never as zero. The LOST row (lost parent attribution: the node table was full or the probe was refused) is listed on its own, never under a caller. `--elf` names functions by reading each `.opd` descriptor's entry word and the FUNC symbol of `.symtab` that contains it (C++ names demangled); without it, or for an address that does not resolve, the raw `fn` value is printed in hex and counted. When both the ELF's `.note.gnu.build-id` and the capture's build id are present they are compared: a mismatch prints a WARNING (names are still shown); otherwise the names are labelled "ELF match unverified".

Pass `--elf` the unstripped linked ELF, not the packaged one. The packaging step strips the `.elf` it places beside the SELF, so that file has no function symbols, and `--elf` given a stripped ELF refuses to symbolize and keeps the raw rows. In a CMake project the unstripped file is the link target's own output (for the heterogeneous-workload sample, `heterogeneous-workload` in the build directory, with no `.elf` suffix); keep it together with the SELF you run, because it must come from the same link.

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
- **Timeline** — PPU threads, SPU lanes and RSX domains as separate rows. SPU captures with order records use the causal-placement and RMAX1 qualifications in section 2.8; unfinished order evidence is shown as PROVISIONAL. Legacy SPU analysis uses the selected W32 or R64 premise. Bars whose timing was extrapolated are flagged. On SPU lanes and RSX domain tracks, shaded stretches labelled **no placed pairs** mark intervals where no placed completed pair's outer extent (earliest begin to latest end) meets that timeframe. This does *not* mean the SPU or RSX was idle: work that was not placed (such as unplaced pairs, censored spans, unpaired events, open spans, loss records, or epochs without an inventory whose events are unknown) may still fall in those stretches.
- **Frames** — per-frame bars measured against a 30 / 60 FPS budget, over-budget frames in red; per-lane tables. The **Graph** selector offers four display modes (also selectable with `--graph`):
  - **Bars** (default): PPU frame duration graph with budget threshold line, and SPU/RSX foreign lane strips below sharing the same slot columns and horizontal scroll.
  - **Lines**: Plots all series (PPU and lanes) on one shared tick axis, with frame k at the same horizontal position for every series. The PPU point is the exact elapsed time. A lane point is drawn at the upper bound (hi) of its conservative included-work bound, with lo..hi rendered as a translucent band (an infinite hi has no line point; its band runs to the top with an open-arrow cap). The line breaks at any slot without a value (unknown, refused, nothing placed, no interval); gaps are never bridged and never drawn as 0. Dots on the baseline mark measured frames with nothing included on that lane.
    - **Smoothing**: Segmented control (**Raw**, **Mean 5**, **Mean 15**, **Mean 30**) applying a trailing moving average (capped at 64 frames) within unbroken runs (fewer frames at run starts, never across gaps); raw lines remain visible faintly.
    - **Scale**: Shared with Distribution (**Fit p99** [default] or **Max**). **Fit p99** sets the top of the axis to 1.1 times the largest of the PPU p99, each lane's p99 upper bound and the budget; a series with fewer than 20 usable frames contributes its maximum instead of a p99. Larger values, +inf upper bounds and bounds not representable as finite values are drawn at the top edge with an upward arrow. If a lane's distribution was refused, only the Fit p99 axis uses that lane's finite maximum instead, with a warning note; the refused distribution itself stays shown as unavailable. **Max** scales so every finite representable value fits; +inf upper bounds and non-representable finite bounds are drawn at the edge with an arrow.
  - **Distribution**: Shows the distribution over measured frames per series (excluded frames are counted, never binned). PPU series shows an exact integer histogram and percentiles (p50, p90, p99; ZoneStats rank rule, refused below 20 measured frames). A lane series gives, per percentile, the enclosing interval [p-th smallest lo, p-th smallest hi] across frames that have a bar (order statistics are monotone). This bounds only retained DEFINITE start-attributed top-level included work on frames with a bar (ambiguous, extrapolated, exclusive-unavailable pairs, and frames with nothing included are omitted; bounds only included work, not total lane activity; refused below 20 frames with a bar).
  - **Verdict map**: Dense matrix with one row per series and one cell per frame, colored by budget verdict or slot state: `OVER`, `WITHIN`, `UNDETERMINED` (striped), `no verdict` (no budget or clock), `verdict unavailable` (exact comparison refused), `nothing included` (dot), `unknown` (outline), `refused` (crossed), or `no interval` (empty cell; no measured interval on lane).
  - **Stated limits**: Series are never summed: lanes run concurrently with the PPU and with each other, and lane values represent start-attributed, unclipped included work, not occupancy (neither stacked totals nor per-frame utilization ratios are established by the reader). Percentiles cover only frames with bars. Edge arrows mark +inf, non-representable or out-of-scale values. A refused lane distribution is shown as unavailable; only the Fit p99 axis falls back to that lane's finite maximum, with a warning.
- **Zones** — named-zone statistics for PPU zones only, from PPU lanes that are MEASUREMENT VALID (`ps3dk-prof report` prints each lane's validity; spans on other lanes are excluded); SPU and RSX zones are not listed. Columns: Zone (name and tag), Instances, Completed, Censored, Total, Mean, p50, p90, p99 and Max (all durations inclusive), and Qualification. Percentiles use the usable completed duration samples and are refused when there are fewer than 20 of them. Unknown values sort last.
- **Hot paths** — the call paths that account for the most measured time, for PPU threads and (when present) SPU lanes and RSX domains. For a capture with automatic function paths, a **Source** selector switches between **Zones** and **Functions (automatic)**; the latter shows one thread's indented call tree (calls, inclusive, self, % of the selection's wall elapsed, row statuses) for the whole capture or one interval, with the counters and statuses above it, the LOST row apart from the tree, and **Load ELF symbols...** for names.

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

A **Capture RSX frame** button next to **Capture full frame** is planned: it will time the supported draw calls of one frame on an RSX domain (safe-form array, index and inline draws and surface clears, from PSL1GHT `rsx*` and cell-style GCM callers), each only when it can be timed safely.  Other calls (the `Unsafe` forms, immediate mode, calls that do not fit the current command-buffer segment or are refused) are counted, not timed, and leave the capture PARTIAL, as can a per-frame draw cap or lost data.  It is a supported-draw timing, not a full frame acquisition.  It is planned to be opted into at build time by linking a hooked `librsx_prof.a` (with the extension archive) instead of `librsx.a`; that archive is not shipped by the normal SDK build described here.

**Status: partial implementation, follow-up feature.** The reader and RSX frames page can inspect supported capture markers already present in a file, and runtime support exists. The live request/button integration and draw-hook packaging are not an available end-to-end capture workflow in this SDK tree. Names, flags and libraries may change before release.

### 5.1 Automatic RSX observation (`PS3_PROF_AUTO`, beta)

A separate, opt-in build mode that needs no profiler code in the application: configure with the SDK's PPU CMake toolchain file and `-DPS3_PROF_AUTO=ON`. That one option selects the automatic providers for every executable in the project (`libps3dkprof_auto.a`, `libgcm_cmd_prof_auto.a` and `libps3dkprof_ext_live.a` in place of `gcm_cmd`, both ABIs), checks them against the sealed `prof-auto.json` in `ppu/lib` (or `ppu/lib/lp64`), writes a link map and a pre-link identity object (`cmake/ps3-prof-auto-identity.py`), and refuses the configure or link when anything does not match. It needs CMake 3.21 or newer with Ninja or Makefiles, and it cannot be combined with `PS3_PROF_WAITS`.

At run time the profiler starts itself, attaches at the application's `cellGcmInit` and writes `/dev_hdd0/tmp/<executable>-<timebase>.prof`. Frames come from the application's flips (`cellGcmSetFlip` / `cellGcmSetPrepareFlip` on the attached context); an explicit `ps3dk_prof_frame` takes precedence. Covered draws and clears (array, index, clear; including draws inside CALL segments built with `cellGcmSegment*`) get reached-marker timestamps, each frame gets RSX begin/end marks, and the capture carries its own RSX clock anchors.

Read the capture with `ps3dk-prof auto [--json] capture.prof`: coverage per frame, reached-marker intervals (they include queued work; they are not isolated draw cost), the capture's own clock map and a per-frame verdict. A frame is PACING when the RSX is not limiting and the frame spent at least a quarter of its interval in observed flip-status waits; it is UNKNOWN, with the missing input named, when the evidence is not there. A frame where the RSX is not limiting and no presentation wait was seen reads UNKNOWN ("PPU work vs presentation pacing not distinguished"): PPU-bound is never reported without busy evidence, which this mode does not collect yet. The final frame is closed at shutdown without an end mark and reads UNKNOWN.

Status: beta, not yet code reviewed. ILP32 has run on hardware; LP64 archives are built and sealed but have not been run on hardware. Design and wire format: `docs/design/ps3dk-prof-auto-rsx.md`.

---

## 6. Troubleshooting

### Verdict words

| Verdict | Meaning |
|---|---|
| `COMPLETE` | The reader found the file structurally complete (header to trailer, every chunk valid).  Whether the run itself finalized cleanly, including the file's close, is not in the bytes: it is the result of `ps3dk_prof_shutdown` on the console (and, for a live session, the agent's END status). |
| `COMPLETE (ended without a trailer ...)` | Valid chunks, no trailer, at least one event chunk: the app quit, the link ended or the copy was cut short.  Reported by `ps3dk-prof` and the viewer alike (exit `0`, extra line `ending: NO-TRAILER`).  Analysis stops at the last whole chunk; a torn last chunk is dropped and its byte count is named in the note. Loss not yet reported at the cut cannot be seen. File acceptance does not establish source validity, clock-mapping qualification or complete-frame evidence. Each source keeps its own loss, open-span, final-record and mapping reasons. SPU order evidence without `ORDER_FINAL` uses the supported PROVISIONAL causal-placement path, without final tick counts or final coverage credit; trailer absence alone does not establish whether `ORDER_FINAL` is present. |
| `INCOMPLETE` | The trailer says the run did not finish cleanly (`NOT_QUIESCED`, `SEQ_LIMIT`, a sink error), or the file has no event chunk at all (a header with nothing captured).  A live session that failed a check (a protocol, integrity or local-copy failure) keeps its copy as `<name>.incomplete.prof`; a missing trailer alone never does. |
| `MALFORMED` | The capture broke a rule: a bad checksum, an out-of-range length, bad structure or inconsistent counts inside the bytes that are there.  Only the missing ending is forgiven; corruption in the prefix is not. |

### A capture that quit from the XMB

An abrupt exit may leave a file without a trailer. If the retained bytes satisfy the accepted-prefix checks and contain at least one event chunk, `ps3dk-prof check` reports `COMPLETE` with the no-trailer ending note and exits `0`; the viewer applies the same file policy. Missing ending bytes alone cannot identify why recording stopped or prove clean shutdown. Extension-source reasons are reported independently: a missing trailer does not imply a missing `DOMAIN_FINAL`. A source with its final record may report only the trailer reason; a source missing both reports both. These facts do not prove that all work in an earlier frame has been recorded. Protocol, integrity and publication failures remain failures, and reopening a kept `.incomplete.prof` does not promote it.

### A live view that never connects

- Both sides on the same subnet, and the PC can open a raw TCP connection to the PS3's IP on `31750` (or the port the agent was started with).
- The capture must be on `/dev_hdd0/`.
- The sample's screen shows the address to connect to, or why the live view is off.

### SPU rows UNMAPPABLE

First check whether the capture contains SPU order records. For those captures, inspect the tick analysis, causal bounds, RMAX1 qualification and any order, identity or budget refusal. For legacy captures without order records, enable W32 only with evidence of no wrap within each epoch, such as the matching complete sample receipt with `w32_eligible=1` and successful lifecycle checks. W32 cannot repair missing observations or loss, and these two analysis paths must not be treated as interchangeable.

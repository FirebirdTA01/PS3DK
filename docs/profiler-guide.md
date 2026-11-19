# Profiler user guide

This guide covers the PS3DK profiler from the console side (the libraries an application links and the API it calls)
and the PC side (the `ps3dk-prof-viewer` app and the `ps3dk-prof` command line). Every option, default and limit here
is taken from the code in this tree; the design documents under `docs/design/` hold the formats and the full contracts.
The complete app-side API (every function, macro, type and result code, with its PC app equivalent) is in the
[profiler API reference](profiler-api.md).

Status: beta. The live API and the SPU/RSX extension are marked EXPERIMENTAL in their headers.

---

## 1. What the profiler is and what it measures

The console runtime (`libps3dkprof` and its variants) records into one capture file per run, `<app>-<16 hex digits>.prof`,
by default under `/dev_hdd0/tmp`. The PC tools read that file: from disk, or while it grows over a live TCP link.

| Source | What is recorded | How it gets into a capture |
|---|---|---|
| PPU zones | Named begin/end spans per PPU thread, on the PPU timebase | `PS3DK_PROF_ZONE*` in app code |
| Frames | One mark per frame on the frame thread | `PS3DK_PROF_FRAME()` in app code, or the app's flips in an AUTO build |
| SPU lanes | SPU zones on each SPU worker, in SPU decrementer ticks | extension archive + SPU lane writer + app code |
| RSX domains | GPU zones between command-buffer reports | extension archive + `ps3dk_prof_gpu_*` calls |
| AUTO (automatic RSX observation) | Per-draw and per-frame RSX reached-marker timing, CALL segments, ranges, per-frame verdicts | `PS3_PROF_AUTO=ON` at configure time |
| FIFO wrap waits | PPU wall time spent in command-buffer wrap phases | FIFO archive pair + `ps3dk_prof_fifo_attach` |
| Counters | Named per-frame integer samples | counters archive + `PS3DK_PROF_COUNTER` |
| Function paths | Automatic PPU call trees from `-finstrument-functions` | func archive + instrumented sources |
| Waits | Elapsed time around pthread mutex and condition waits | `PS3_PROF_WAITS` + observed archives |
| Live | The capture streamed to the PC app while it is written | `ps3dk_prof_live_start` in app code |

**Capture windows.** The profiler records nothing heavy (zones, counters, waits, GPU zones, AUTO draw timing, ranges)
unless a capture window is open. Frame marks and clock anchors run all the time, so frame numbers stay run-wide. A run
with no window produces a valid capture with no heavy data. A window is opened by app code (section 5.4) or by the PC
app over a live link (section 6.5). No build opens one automatically.

Pieces:

| Piece | Role |
|---|---|
| `libps3dkprof.a` | Manual-lifecycle PPU runtime (the app calls init/shutdown). Contains the live agent. |
| `libps3prof.a` | Automatic-lifecycle PPU runtime: initializes in a constructor before `main`, shuts down after it. No live agent. |
| `libps3dkprof_ext.a`, `libps3dkprof_ext_live.a`, `libps3dkprof_spu.a` | SPU-lane and RSX-domain extension, its live agent, and the SPU lane writer. |
| Feature variants | Counters, function paths, waits, FIFO and AUTO archives (section 4). |
| `ps3dk-prof-viewer` | The PC app (section 6). |
| `ps3dk-prof` | The command-line checker and reporter (section 7). |

---

## 2. Requirements

**Retail and HEN (non-DEX) consoles: the application must link the profiler.** There is no system-side profiler on
these consoles, so every capture comes from profiler code linked into the application itself. Without a profiler
archive in the link there is nothing to capture and nothing for the PC app to connect to.

Support for DEX consoles without linking the profiler is planned. It is not available in this release.

**Which archive to link.** Pick one row. Each archive is installed for both PPU ABIs: ILP32 in `$PS3DK/ppu/lib`,
LP64 in `$PS3DK/ppu/lib/lp64`. Link the one that matches the app's ABI.

| You want | Link (in place of nothing or of the row above it) | Installed by `make -C sdk install`? |
|---|---|---|
| PPU zones, frames, windows, live | `-lps3dkprof` | yes |
| Same, without writing init/shutdown | `libps3prof.a` with `-Wl,-u,__ps3dk_prof_startup_anchor` (no live) | yes |
| + SPU lanes and RSX domains | `libps3dkprof_ext.a` instead of `libps3dkprof.a`; for live also `libps3dkprof_ext_live.a`, before it; SPU side `libps3dkprof_spu.a` | yes |
| + counters | `libps3dkprof_counters.a` instead of `libps3dkprof.a`, or `libps3dkprof_ext_counters.a` instead of `libps3dkprof_ext.a` | no: `make -C sdk/libps3dkprof install-counters` |
| + function paths | `libps3dkprof_func.a` instead of `libps3dkprof.a`, or `libps3dkprof_ext_func.a` instead of `libps3dkprof_ext.a` | no: `install-func` / `install-func-ext` |
| + waits | chosen by `PS3_PROF_WAITS` (section 4.6) | no: `make ... waits` with measured inputs |
| + FIFO wrap waits | chosen by `ps3_target_prof_fifo` (section 4.7) | no: `install-fifo-package` |
| AUTO RSX observation | chosen by `PS3_PROF_AUTO=ON` (section 4.8) | no: `install-auto-package` |

The manual-lifecycle variants of counters, function paths and waits already contain the live agent; their extension
forms need `libps3dkprof_ext_live.a` for live, like `libps3dkprof_ext.a`.

**For the live link** also link `net` (the sample links `net netctl_stub`; `netctl` is used only to show the console's
IP address) and call `netInitialize()` before `ps3dk_prof_live_start`, or turn on live auto-start (section 4.3), which
needs no app code.

**CMake.** There is no helper for the manual and extension archives: name them in `target_link_libraries`. Example
for an ILP32 app with the live link:

```cmake
target_compile_definitions(my-app PRIVATE PS3DK_PROF=1)
target_link_libraries(my-app PRIVATE ps3dkprof net netctl_stub
                      gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m)
```

AUTO, FIFO and waits have CMake selectors that substitute the archives for you (sections 4.6 to 4.8). They need CMake
3.21 or newer and the Ninja or Makefiles generator.

**Make / manual link lines.**

```
ILP32 : powerpc64-ps3-elf-gcc ... -DPS3DK_PROF=1 ... -L$PS3DK/ppu/lib       -lps3dkprof -lnet -lrt -llv2 -lm
LP64  : powerpc64-ps3-elf-gcc ... -DPS3DK_PROF=1 ... -L$PS3DK/ppu/lib/lp64  -lps3dkprof -lnet -lrt -llv2 -lm
```

The extension archives carry a data-model tag; `samples/profiling/prof-ext-abi.cmake` checks at configure time that
the archive matches the app's ABI. An app can reuse that check.

---

## 3. Quick start

The shortest path from source to a capture in the PC app, with a plain `libps3dkprof.a` build.

**1. Add the profiler to the app.**

```c
#include <ps3dk/prof.h>
#include <net/net.h>

int main(void)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "my-app";                         /* names the capture file */
    int prof = ps3dk_prof_init(&cfg) == PS3DK_PROF_OK;
    PS3DK_PROF_THREAD_BEGIN("main");            /* zones from unregistered threads are ignored */

    int live = 0;                               /* optional: the PC app can connect */
    if (prof && netInitialize() == 0) {
        if (ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK) live = 1;
        else netDeinitialize();
    }

    PS3DK_PROF_CAPTURE_WHOLE_RUN();             /* record from here to shutdown; omit it to start
                                                   recording from the PC app instead (section 6.5) */
    while (running) {
        PS3DK_PROF_ZONE("update");              /* ends at the end of this block */
        update();
        render();
        PS3DK_PROF_FRAME();
        PS3DK_PROF_DRAIN();                     /* APP mode: drain once per frame */
    }

    ps3dk_prof_shutdown();                      /* before live_stop: the viewer needs its END */
    if (live) {
        int rc;
        do rc = ps3dk_prof_live_stop(2000000, 1000000);
        while (rc == PS3DK_PROF_ETIMEDOUT || rc == PS3DK_PROF_ECLOCK);
        if (rc == PS3DK_PROF_OK) netDeinitialize();
    }
    return 0;
}
```

Build with `-DPS3DK_PROF=1` and link `ps3dkprof net` (section 2).

**2. Run it** on the console (HEN or RPCS3 for the file path; the live link has been run on a HEN console).

**3. Look at it on the PC.** Either:

- live: start `ps3dk-prof-viewer`, press **Connect…**, enter the console's IP address and port `31750`, and a folder
  for the saved copy; or
- from the file: copy `/dev_hdd0/tmp/my-app-<16 hex>.prof` off the console (FTP), then
  `ps3dk-prof check my-app-....prof` and `ps3dk-prof-viewer my-app-....prof`.

A complete example with SPU and RSX work is `samples/profiling/heterogeneous-workload` (options `SAMPLE_PROFILING`,
`SAMPLE_LIVE`, `SAMPLE_PROF_EXT`, `SAMPLE_FUNC_PATHS`, `SAMPLE_FIFO_EVENTS`; see its README). An AUTO example is
`samples/gcm/hello-ppu-prof-auto-verdicts`.

---

## 4. Opting in to features with build options

Every feature below is off unless selected. Each one says what it costs and when to turn it on.

### 4.1 `PS3DK_PROF` (compile define)

`-DPS3DK_PROF=1` on every translation unit that uses `<ps3dk/prof.h>`. With it undefined or `0`, every `PS3DK_PROF_*`
macro is inert: arguments are not evaluated, no profiler function is referenced, `PS3DK_PROF_ZONE_BEGIN` yields an
INERT handle, and `PS3DK_PROF_LIVE_START` returns `PS3DK_PROF_ENOTSUP`. Types and result codes are declared in both
builds, so zone calls can stay in the source. The profiler is PPU-only through this header: an SPU translation unit
with `PS3DK_PROF` set fails to build.

- Turn it on when: you build a profiled configuration of the app.
- Leave it off when: you ship, or measure the app without any profiler code.

The capture-window macros (section 5.4) are also live in an AUTO build (`PS3TC_PROF_AUTO=1`, set by the AUTO
selector) without `PS3DK_PROF`.

### 4.2 Runtime configuration (`ps3dk_prof_config`)

Start from `ps3dk_prof_config_defaults(&cfg)` and change fields before `ps3dk_prof_init`.

| Field | Default | Range | Notes |
|---|---|---|---|
| `mode` | `PS3DK_PROF_MODE_APP` (1) | APP or `PS3DK_PROF_MODE_THREAD` (2) | APP: the app calls `PS3DK_PROF_DRAIN`. THREAD: an internal thread drains. |
| `max_threads` | 64 | 1..1024 | thread slots |
| `max_descriptors` | 1024 | 1..65536 | one per zone site |
| `ring_records` | 4096 | power of two, 512..1048576 | per-thread record ring |
| `drain_interval_us` | 10000 | 1000..1000000 | THREAD mode only |
| `drain_stack_bytes` | 65536 | 16 KiB..1 MiB | THREAD mode only |
| `quiesce_timeout_us` | 1000000 | 0..60000000 | how long shutdown waits for recording threads |
| `auto_register` | 0 | 0 or 1 | 1: a thread is registered on its first zone |
| `app` | `"app"` | 1..32 of `[A-Za-z0-9_-]` | names the capture file |
| `build_id`, `flags`, `workload_tag` | NULL | strings | recorded in the capture |

THREAD mode:

- Turn it on when: the app has no single place to drain each frame, or the drain must stay off the render thread;
  FIFO observation requires it.
- Leave it off when: you want the drain cost at a known point in the frame (APP mode drains where you call it).

`auto_register`:

- Turn it on when: zones run on threads you do not create yourself (library worker threads).
- Leave it off when: you register threads with `ps3dk_prof_thread_begin`; with 0, zones from unregistered threads
  are ignored.

Measured cost (console, heterogeneous-workload sample, ILP32): one PPU zone pair about 0.5 us; PPU profiling 10 to 50
us per frame; with SPU and RSX zones 60 to 90 us per frame; the per-frame drain about 0.2 ms, up to 1.6 ms in phases
with heavy SPU zone traffic.

### 4.3 Live link

Linked with `libps3dkprof.a` (or `libps3dkprof_ext_live.a` in extension builds) and started by app code (section 5.6),
or with no app code by the opt-in auto-start below.

- Cost: one agent thread (64 KiB stack by default), a listening socket, and file reads while a viewer is connected.
  The writer never waits for the viewer. Overhead on the console has not been measured.
- Turn it on when: you want to watch a run while it happens, or start and stop capture windows from the PC.
- Leave it off when: the app has no network, the capture uses a callback sink, the capture is not on `/dev_hdd0/`,
  or it uses rolling output (all refused with `ENOTSUP`).

#### Live viewer auto-start (`PS3_PROF_LIVE_AUTOSTART`, opt-in)

With auto-start, the PC app can connect to an app that has no live or network code. **It is off by default.** Every
profiler build can turn it on: plain `PS3DK_PROF` builds (`libps3dkprof.a`), the classic automatic lifecycle
(`libps3prof.a`), the extension and its FIFO/function/counter forms (with `libps3dkprof_ext_live.a`), `PS3_PROF_WAITS`
and `PS3_PROF_AUTO`.

- **CMake:** `-DPS3_PROF_LIVE_AUTOSTART=ON`. Every executable that links the profiler gets the link request, `net` and
  `sysmodule_stub`; an extension or `PS3_PROF_AUTO` executable also gets `libps3dkprof_ext_live.a`. Optional:
  `PS3_PROF_LIVE_PORT` (default `31750`) and `PS3_PROF_LIVE_NET_POOL_KB` (default `0`, see below).
- **Without CMake:** compile one unit that includes `<ps3dk/prof.h>` with `-DPS3DK_PROF_LIVE_AUTOSTART=1` (or link with
  `-Wl,-u,__ps3dk_prof_live_autostart_anchor`) and link `-lnet -lsysmodule_stub` after the profiler. A missing library
  is a link error, never a silent off. `PS3DK_PROF_LIVE_AUTOSTART_OPTIONS(port, pool_kib);`, written once at file
  scope, sets the port and pool and also requests auto-start.

What the runtime does:

1. After every successful `ps3dk_prof_init` it creates one short-lived start-up thread (64 KiB stack). In the automatic
   lifecycles init runs in a constructor; the thread keeps the network work out of it.
2. The thread checks that the capture can be served (a FILE sink under `/dev_hdd0/`, no rolling), takes the profiler's
   network hold and starts the agent as `ps3dk_prof_live_start(NULL)` would. The TTY shows
   `ps3prof: live viewer: connect to this console on port 31750`.
3. Any failure prints one `ps3prof: live auto-start off: ...` line with the reason (no servable capture, network
   bring-up failed, port taken, thread not created) and profiling continues unchanged.
4. After `ps3dk_prof_shutdown` has written the finalization, the runtime stops the agent, so a connected viewer gets
   `END` (up to 2 s for the viewer, 1 s for the thread, then 2 s more if needed). It releases its network hold only
   after the stop succeeded; an agent that does not stop keeps it until the process exits, with one line saying so.
5. A manual-lifecycle program that returns from `main` without `ps3dk_prof_shutdown` is covered by a late destructor:
   the agent is cancelled (no `END`; nothing was finalized) and the hold released.

**The app's network stays the app's.** With auto-start off, or while the profiler holds no network, every
`netInitialize`, `netDeinitialize`, `sys_net_initialize_network_ex` and `sys_net_finalize_network` call behaves exactly as
before. While the profiler holds the network, libnet applies these rules:

- The network belongs to whoever brought it up, and so does its memory pool.
- If the app brought it up first, the profiler joins without a call of its own. When the app tears it down
  (`netDeinitialize`, `sys_net_finalize_network`), the profiler is told first: the agent stops at once (a connected
  viewer sees a disconnect, not `END`), the profiler drops its hold and never touches the network again, and the app's
  call then reaches the system exactly as before. Live viewing ends there, with one `ps3prof: live auto-start stopped`
  line. The stop holds the app's call for at most about 3 s (1 s, then 2 s more, for the agent thread). If the agent
  still has not stopped (for example, it is inside a file read), the app's call goes ahead anyway with one
  `... the agent did not stop ...` line, and the agent's sockets can stay open for a while after the app's finalize,
  until that thread leaves its loop; its socket calls then fail rather than reach a live network.
- A `sys_net_finalize_network` from an app that never called `sys_net_initialize_network_ex` goes straight to the
  system with its old result, and the profiler keeps its hold (if that call tears down the network the profiler is
  using, the agent's connection simply fails).
- If the profiler brought it up first (with its own static pool, by default the 128 KiB `netInitialize` pool), a later
  `netInitialize` joins it and the matching `netDeinitialize` leaves it up for the profiler. A later
  `sys_net_initialize_network_ex` brings the app's own pool: the profiler steps aside first (agent stopped, its network
  finalized, the NET module left loaded, one TTY line from libnet) and the app's call goes straight to the system. If
  a `netInitialize` had joined first, the profiler instead hands the network to it (agent stopped, network left up),
  so that call and the later `netDeinitialize` give the results the same sequence gives without the profiler.
- The profiler never finalizes a network an app front end or the ps3dk-run hook brought up, and never unloads a module
  it did not load.
- If something outside libnet brought the network up, the profiler's bring-up fails and auto-start stays off with its
  one line.

**Existing `ps3dk_prof_live_start` calls keep working.** While the auto-started agent runs, the call returns `OK` and
does nothing when every non-zero field of its config matches, `EALREADY` otherwise. Before the auto-started agent is
up (its start-up thread is still bringing the network up) or after auto-start failed, the app's own start behaves as
always, and the auto-start then backs off. The app may also stop the auto-started agent; the runtime then only
releases its hold. Shutdown waits at most 5 s for the start-up thread; one still inside the network bring-up is left
to finish on its own (one TTY line) and gives its hold back when it does.

- Cost (on top of the live link above): the start-up thread at init; the libnet pool (by default the 128 KiB
  `netInitialize` pool, shared with an app that also uses `netInitialize`) and the NET module; a listening socket on
  every interface for the whole run; a wake-up every 10 ms; and, only while a viewer is still downloading, up to about
  3 s (5 s at worst) added to shutdown.
- Turn it on when: you want to watch or control a development or test build from the PC, especially an app whose
  source you cannot or do not want to change; RPCS3 or HEN sessions with the viewer.
- Leave it off when: the build ships; overhead and benchmark runs (the wake-ups and file reads perturb timing); the
  network is not trusted (the listener is unauthenticated: anyone who reaches the port can read the capture); the app
  needs a larger libnet pool and you have not set the pool option; or the capture cannot be served (callback sink,
  rolling, outside `/dev_hdd0/`; it only prints why).

### 4.4 SPU lanes and RSX domains (extension, EXPERIMENTAL)

Link `libps3dkprof_ext.a` instead of `libps3dkprof.a` (and `libps3dkprof_ext_live.a` first for live). The extension
headers `prof_spu.h`, `psp_desc.h` and `prof_gpu.h` are installed in `ppu/include/ps3dk`. The app initializes with
`ps3dk_prof_init_ex`, registers SPU images, creates SPU lanes (`ps3dk_prof_spu_lane_create`) and links the SPU side
with `libps3dkprof_spu.a`; RSX zones come from `ps3dk_prof_gpu_attach` and `PS3DK_PROF_GPU_ZONE_BEGIN/END`. The
heterogeneous-workload sample (`SAMPLE_PROF_EXT=ON`) is the reference for this lifecycle.

- Cost: one SPU zone pair costs about 21 SPU decrementer ticks; SPU traffic raises the drain cost (section 4.2).
- Turn it on when: you need SPU job timing or app-defined GPU zones beside the PPU timeline.
- Leave it off when: PPU zones are enough, or you want RSX timing without app code (use AUTO instead).

SPU zone durations are SPU decrementer ticks, never wall time. Placement on the PPU timeline uses causal bounds and the
RMAX1 rate premise; see the Timeline notes in section 6.3. SPU zones inside SPURS jobs have not been tested.

### 4.5 Counters and function paths

**Counters.** Build with `make -C sdk/libps3dkprof install-counters`; link `libps3dkprof_counters.a` (or
`libps3dkprof_ext_counters.a`). `PS3DK_PROF_COUNTER(name, value)` records one sample per call; at most 64 names, 4096
samples held between drains. Each call takes one mutex.

- Turn it on when: you want per-frame values (draw counts, memory, queue depth) next to the timing.
- Leave it off when: you do not emit counters; the default archives never set the counters flag.

**Function paths.** Build with `install-func` (or `install-func-ext`); link `libps3dkprof_func.a` (or
`libps3dkprof_ext_func.a`) and compile the sources you want traced with `-finstrument-functions`. Optional limits:
`ps3dk_prof_func_configure` before init (`<ps3dk/prof_func.h>`).

- Cost: about 0.43 us of added elapsed time per instrumented entry in one ILP32 console comparison (a workload
  comparison, not an isolated hook cost).
- Turn it on when: you need call trees without writing zones.
- Leave it off when: tight leaf functions dominate; exclude them with `-finstrument-functions-exclude-file-list` or
  `__attribute__((no_instrument_function))`, and keep the same selection when comparing captures.

Start the profiler before entering the functions you need: a function that enters before init and exits after it
counts as an orphan and adds no path.

### 4.6 `PS3_PROF_WAITS` (CMake)

Values `OFF` (default), `MANUAL` (`libps3dkprof_wait.a`), `AUTO` (`libps3prof_wait.a`, the automatic-lifecycle form)
or `EXT` (`libps3dkprof_ext_wait.a`). `PS3_PROF_WAITS_ROOT` (default `$PS3DK`) names the prefix holding them and the
observed runtime redirect `ppu/lib[/lp64]/prof-waits/librt.a`. These are not part of the normal install: they are
built by `make -C sdk/libps3dkprof waits` with measured `WAIT_*` inputs and `scripts/build-prof-wait-runtime.py`.
Records elapsed time around pthread mutex lock, timed mutex lock and condition wait/timedwait.

- Turn it on when: you suspect lock or condition-variable waits.
- Leave it off when: building with `PS3_PROF_AUTO` or the FIFO selector (both refuse the combination).

Timed mutex calls are recorded only after their initial trylock reports busy; immediate successes are not covered.
See `docs/design/ps3dk-prof-waits.md`.

### 4.7 FIFO wrap waits (`ps3_target_prof_fifo`)

Install with `make -C sdk/libgcm_cmd install-fifo` then `make -C sdk/libps3dkprof install-fifo-package`. In CMake,
after `project()` and after adding the target's libraries:

```cmake
include(${PS3DK}/cmake/ps3-prof-fifo.cmake)
ps3_target_prof_fifo(my-app ROOT ${PS3DK})
```

It replaces `gcm_cmd` with `libgcm_cmd_prof_fifo.a` and `ps3dkprof_ext` with `libps3dkprof_ext_fifo.a`. The app runs
in THREAD mode and calls `ps3dk_prof_fifo_attach(gpu, ctx, NULL, &instance)` on the GCM context owner after attaching
the GPU domain. Default ring: 1024 events of 96 bytes, ceiling 1 MiB.

- Turn it on when: frames stall in command-buffer wraps.
- Leave it off when: you use AUTO (it already contains FIFO observation) or waits (refused together).

Events are PPU wall time between wrap-protocol boundaries, never GPU busy or idle time. See
`docs/design/ps3dk-prof-fifo.md`.

### 4.8 `PS3_PROF_AUTO` (CMake, beta)

Automatic RSX observation. Install the providers once, against the same headers and toolchain as the SDK:

```sh
make -C sdk/libgcm_cmd install-auto
make -C sdk/libps3dkprof install-auto-package
```

Then configure with the SDK's PPU toolchain file and `-DPS3_PROF_AUTO=ON`. `PS3_PROF_AUTO_ROOT` (default `$PS3DK`)
names a different prefix that holds the providers. The option:

- defines `PS3TC_PROF_AUTO=1` for every C/C++ unit of the project;
- for every executable, replaces `gcm_cmd` with `libgcm_cmd_prof_auto.a` and `ps3dkprof_ext` with
  `libps3dkprof_auto.a`, adding them when the target does not link them, through static libraries it links;
- keeps `ps3dkprof_ext_live` when the app links it (it is never added automatically);
- checks the providers against the sealed `prof-auto.json`, writes a link map and an identity record
  (`<map>.identity.json`), and refuses the configure or link when anything does not match.

Refused: any other profiler or GCM archive in the link (`ps3dkprof`, `ps3prof`, a default `gcm_cmd` named inside a
generator expression), a non-GLOBAL imported target holding one, `PS3_PROF_WAITS`, a target already selected for FIFO,
and a target that already has a link launcher.

At run time the profiler initializes itself (THREAD mode) before `main`, attaches at the app's `cellGcmInit`, takes
frames from the app's flips and writes `/dev_hdd0/tmp/<executable>-<timebase>.prof`. **It still needs a capture
window**: add `PS3DK_PROF_CAPTURE_WHOLE_RUN();` (or another window, section 5.4) to the app, or nothing heavy is
recorded. Call `PS3DK_PROF_AUTO_SEAL()` before any exit path that skips `atexit` handlers.

It uses local reports 1024..2047 and labels 246 and 247 (`docs/gcm-labels.md`); the app keeps labels 64, 69..239 and
248..254 and reports 512..1023. ILP32 has run on hardware; LP64 archives are built and sealed but not run on hardware.

- Turn it on when: you want RSX timing and per-frame verdicts without writing GPU zones.
- Leave it off when: the app uses waits, links its own profiler archive, or writes command buffers through raw
  writers the profiler cannot observe (those are outside coverage).

Optional build-time hooks can be disabled per unit with `PS3TC_PROF_AUTO_NO_FLIP_HOOK`, `PS3TC_PROF_AUTO_NO_FP_HOOK`
and `PS3TC_PROF_AUTO_NO_LABEL_HOOK`. Design: `docs/design/ps3dk-prof-auto-rsx.md`; tags and ranges:
[profiler-auto-tags.md](profiler-auto-tags.md).

### 4.9 `PS3_PROF_AUTO_RING`, `PS3_PROF_AUTO_RING_MB`, `PS3_PROF_AUTO_RING_RECORDS`

With `PS3_PROF_AUTO=ON`. Default OFF. `_MB`: MAIN extent size, 1..256, default 16. `_RECORDS`: operation records,
1..65536, default 16384. The selector compiles `<target>-auto-ring.c` into each executable and requires its symbol at
link time.

Without the ring, timed draws per frame are bounded by the local report pool (504 pairs, less the whole-CALL and range
reserves); later draws of the frame are counted as untimed ("report capacity"). The ring copies owner-buffer draw
reports in blocks of 126 pairs to MAIN memory and recycles them.

- Cost: allocated once per process in whole 1 MiB pages: 20 MiB at the defaults (16 MiB extents plus metadata), 3 MiB
  with `_MB=2` and 4096 records.
- Turn it on when: busy frames exceed about 504 timed draws, you need per-draw or per-pass RSX cost, or per-tag
  totals without untimed draws.
- Leave it off when: memory is tight or you are measuring memory, frame-level verdicts are enough, or draws come
  mostly from CALL segments (they keep the local pool either way).

Sizing: one MiB holds 256 blocks (32,256 draws). Size records and extents for the owner-buffer draws outstanding over
about three frames; the defaults cover about 5,000 such draws per frame. Too small shows as draws counted under
`ring_full` or "metadata capacity", or a capture reading `FINAL (lower bounds)`: raise the matching option.
`PS3DK_PROF_AUTO_OPTIONS_SET` before `cellGcmInit` overrides these values.

Recommended ring when you only need the RSX frame capture (4.12): `_MB=1` with `_RECORDS=4096`, about 2 MiB of pages
(1 MiB of extents, 4096 records of 184 bytes, 256 extent descriptors), enough for about 1,300 draws a frame across
three frames of RSX lag. The record count, not the extent size, sets most of the ring's memory: 16,384 records alone
take 2.9 MiB.

### 4.10 `PS3_PROF_AUTO_ROLLING` (CMake)

With `PS3_PROF_AUTO=ON`. Empty (default) keeps an append capture. A value is the file ceiling in MiB, 24..4096
(`-DPS3_PROF_AUTO_ROLLING=64`). The selector generates `<target>-auto-rolling.c` with the finite preset
(`PS3DK_PROF_ROLLING_AUTO_FIFO_INIT`); the capture is then a fixed-size rolling file that keeps the most recent
segments.

- Cost: the preset keeps six segments plus a spare, uses about 21 MiB of file space and about 16 MB of pre-allocated
  writer memory, and rewrites one segment slot per commit with two sync operations. A commit clears only the
  overwritten prefix of the slot and extends the used-data CRC over the known-zero padding without rescanning it (the
  stored bytes and the two-sync publication order are unchanged; readers still check the padding). Target commit
  timings after that change are pending: measure drainer time, lag and dropped records on the target before using
  rolling for a heavy workload. Core/extension/writer state has a separate 64 MiB ceiling.
- Turn it on when: long sessions could fill the HDD, soak tests or playtests where only the recent past matters, or a
  capture must stay within a fixed disk budget.
- Leave it off when: you want whole-run totals or every frame, you record SPU lanes (refused), you need the live link
  (refused with `ENOTSUP`), or the extra memory and writes are unwanted.

Raising the ceiling alone does not enlarge the preset's segments. A fixed byte budget does not promise a fixed number of
seconds. A hard hang can lose the pending segment (up to about 1 MiB). Reaching a semantic or metadata budget stops the
capture at a whole chunk with `PS3DK_PROF_ERESOURCE`. Custom budgets: leave the option empty and define
`ps3dk_prof_automatic_rolling_options` with `PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS` in an executable source (not
in a static archive). Rolling output is available only in AUTO builds; other archives return `ENOTSUP` for an enabled
rolling request. Contract and measurements: `docs/design/ps3dk-prof-rolling-runtime.md`.

### 4.11 Completed-file retention

`ps3dk_prof_set_capture_retention(N)` before init keeps the newest `N` (1..1024) completed captures of this app in the
capture directory; 0 (default) disables it. FILE sinks only.

- Cost: extra shutdown IO (reads and checks old files before deleting) and a bounded 25 KiB index buffer.
- Turn it on when: repeated runs would otherwise accumulate captures.
- Leave it off when: you keep captures by hand, or the app is an AUTO build (the setter runs after AUTO's
  pre-`main` init, so it is too late there).

Untracked, active and incomplete files are never touched. `PS3DK_PROF_ERETENTION` means the capture closed but cleanup
failed. A crash can leave `.ps3dk-retain-<app>.lock`; cleanup then refuses until it is removed by hand (only when no
process uses the directory).

---

### 4.12 `PS3_PROF_RSX_FRAME`, `PS3_PROF_RSX_FRAME_STAGING_KB` (CMake, beta)

With `PS3_PROF_AUTO=ON` and `PS3_PROF_AUTO_RING=ON` (refused without the ring). Default OFF.
`_STAGING_KB`: each of the two staging blocks, 4..1024, default 256. The selector compiles
`<target>-auto-rsx-frame.c` into each executable and requires its symbol at link time.

One requested frame of the AUTO owner: every draw and clear of its command buffer in submission order, with its RSX
time (the AUTO operation that timed it), kind, primitive, vertex or index counts, the vertex and fragment program it
used (named by a canonical hash of their instructions, stable across boots, load addresses and uniform values), its
tag, an app label (5.7) and the AUTO range it ran in. The captured frame times every covered draw, whatever
`PS3DK_PROF_AUTO_TIMED_RANGES_ONLY` says. This release does not record render state, textures, buffers or constant
values.

```cmake
-DPS3_PROF_AUTO=ON -DPS3_PROF_AUTO_RING=ON -DPS3_PROF_AUTO_RING_MB=1 -DPS3_PROF_AUTO_RING_RECORDS=4096 -DPS3_PROF_RSX_FRAME=ON
```

- Cost: the ring (2 MiB at the sizes above); about 64 KiB of decoder state from `cellGcmInit`; while a capture is being
  written, the two staging blocks (512 KiB at the default), freed when it is written; one pass of the profiler over
  the owner's command words, at each covered draw and at wraps and flushes. The captured frame itself runs slower by
  its per-draw records.
- Turn it on when: you need to know which draws, programs or labelled entities take a frame's RSX time, beyond the
  pass totals AUTO ranges give.
- Leave it off when: you are measuring memory, or pass and frame totals are enough. Off, nothing is generated or
  linked, no memory is reserved, each AUTO hook costs one load and a branch, and the capture file is the same as an
  AUTO build without the option (tested byte for byte outside its run-dependent fields).

Files with a capture set file_flags bit 10; a reader older than this release refuses them.

## 5. Hard-coding features in app code

This section shows the common calls. Every entry, with its parameters, results, threading rules and PC app
equivalent, is in the [profiler API reference](profiler-api.md).

### 5.1 Init and shutdown

```c
ps3dk_prof_config cfg;
ps3dk_prof_config_defaults(&cfg);
cfg.mode = PS3DK_PROF_MODE_THREAD;     /* or keep APP and call PS3DK_PROF_DRAIN() */
cfg.app  = "my-app";
ps3dk_prof_set_sink_path("/dev_hdd0/tmp/captures");   /* optional: a DIRECTORY, before init */
if (ps3dk_prof_init(&cfg) != PS3DK_PROF_OK) {
    /* the app runs without a capture; zone calls stay harmless */
}
...
int rc = ps3dk_prof_shutdown();        /* waits up to quiesce_timeout_us, drains, writes the trailer */
```

Before init, `ps3dk_prof_set_sink_path(dir)` names the directory (file `<dir>/<app>-<16 hex>.prof`, created
exclusively, `EEXIST` if present) and `ps3dk_prof_set_sink_callbacks(&cbs)` replaces the file with `{write, flush,
close, ctx}` callbacks (no capture record, no live link). `ps3dk_prof_get_capture_record(&rec)` returns the file path,
session id, app and build id after a successful init.

`ps3dk_prof_init`, `ps3dk_prof_live_start`, `ps3dk_prof_shutdown` and `ps3dk_prof_live_stop` are caller-serialized:
never call two of them at the same time. The automatic-lifecycle archive `libps3prof.a` and AUTO builds do init and
shutdown for you.

### 5.2 Zones

```c
ps3dk_prof_zone h = PS3DK_PROF_ZONE_BEGIN("compute");
compute();
PS3DK_PROF_ZONE_END(h);

{
    PS3DK_PROF_ZONE("compute");        /* C: cleanup attribute; C++: ps3dk_prof::zone_guard */
    compute();
}
```

Names are string literals (one static descriptor per site). `_FOR` variants (`PS3DK_PROF_ZONE_BEGIN_FOR(name, tok)`,
`PS3DK_PROF_ZONE_FOR`) tie a zone to a frame token from `PS3DK_PROF_FRAME_TOKEN()`. A zone begun outside a capture
window returns an INERT handle and records nothing.

### 5.3 Threads and frames

- `ps3dk_prof_thread_begin("name")` before a thread's first zone, `ps3dk_prof_thread_end()` before it exits (or set
  `auto_register`).
- `PS3DK_PROF_FRAME()` once per frame on the frame thread. The Frames page, frame budgets, `CAPTURE_FRAMES` windows
  and live triggers all depend on it. AUTO builds mark frames from the app's flips; an explicit call takes
  precedence.
- APP mode: the thread that called `ps3dk_prof_init` is the drain thread. Call `PS3DK_PROF_DRAIN()` and
  `ps3dk_prof_shutdown()` from it (from any other thread they return `EPERM`). To keep the drain off the render
  thread, initialize the profiler on a separate PPU thread and drain from there. In THREAD mode
  `PS3DK_PROF_DRAIN()` returns `EBUSY`: the internal thread is the only drainer.

### 5.4 Capture windows

One window is open at a time. A window belongs to the thread that marks frames; one begun before any frame mark is
adopted by the first thread that marks one.

```c
PS3DK_PROF_CAPTURE_WHOLE_RUN();            /* from here to shutdown: the simplest choice */

PS3DK_PROF_CAPTURE_BEGIN("level-2");       /* opens at the next frame boundary */
...
PS3DK_PROF_CAPTURE_END();                  /* closes after the current frame */

PS3DK_PROF_CAPTURE_FRAMES("boss", 120);    /* closes itself after 120 frames (reason COUNT) */

{
    PS3DK_PROF_SECTION("load");            /* begins here, ends with the block */
    load_level();
}

ps3dk_prof_capture_info info;
PS3DK_PROF_CAPTURE_STATE(&info);           /* state, id, kind, name, frames, refused, queue_full */
```

Results: `EBUSY` while a window is open or about to open (counted); `EFULL` when the drainer is too far behind to record
the window's begin and end (nothing opens); `EINVAL` for `FRAMES(0)` or `END` with no window; `EPERM` for `END` from a
thread that neither owns nor began the window. At shutdown an open window closes with reason SHUTDOWN, or SEAL through
`ps3dk_prof_capture_seal()` / `PS3DK_PROF_AUTO_SEAL()`. An app window blocks the PC app's Begin and Capture N frames
(they are refused `EBUSY`) for as long as it is open. SPU lane records and function paths are not gated by windows.

### 5.5 AUTO tags, ranges and options

In an AUTO build, on the owner thread (`<ps3dk/prof_auto.h>`; all inert without `PS3_PROF_AUTO`):

```c
PS3DK_PROF_AUTO_TAG(2);                         /* later operations carry tag 2 (Passes page) */
if (PS3DK_PROF_AUTO_RANGE_BEGIN("shadows")) {}  /* RSX time between BEGIN and END */
draw_shadows();
PS3DK_PROF_AUTO_RANGE_END();                    /* always issue END, whatever BEGIN returned */
PS3DK_PROF_AUTO_TIMED_RANGES_ONLY();            /* per-draw timing only inside ..._BEGIN_TIMED ranges */
PS3DK_PROF_AUTO_SEAL();                         /* before _exit or a quit-guard deadline */
```

Ranges nest to depth 4 and use a reserve of report pairs (default 63). `PS3DK_PROF_AUTO_OPTIONS_SET(&opts)` before
`cellGcmInit` (same thread) replaces the default report range, labels and ring fields; start from
`PS3DK_PROF_AUTO_OPTIONS_INIT`. Details: [profiler-auto-tags.md](profiler-auto-tags.md).

### 5.6 Live start and stop

```c
if (netInitialize() == 0) {
    ps3dk_prof_live_config lc = {0};
    lc.port = 31750;                            /* zero fields take the defaults */
    if (ps3dk_prof_live_start(&lc) != PS3DK_PROF_OK)
        netDeinitialize();                      /* no agent thread exists */
}
...
ps3dk_prof_shutdown();                          /* first: a connected viewer needs END */
int rc;
do rc = ps3dk_prof_live_stop(2000000, 1000000); /* graceful_us, join_us */
while (rc == PS3DK_PROF_ETIMEDOUT || rc == PS3DK_PROF_ECLOCK);
if (rc == PS3DK_PROF_OK) netDeinitialize();     /* after ETHREAD leave the network up */
```

`ps3dk_prof_live_config` (defaults when a field is 0 or the pointer is NULL):

| Field | Default | Range |
|---|---|---|
| `port` | 31750 | any TCP port |
| `poll_us` | 10000 | 1000..1000000 |
| `send_timeout_us` | 2000000 | 1000..60000000 |
| `handshake_timeout_us` | 2000000 | 1000..60000000 |
| `stack_bytes` | 64 KiB | 16 KiB..1 MiB |
| `cap_bytes` | 1 GiB | 1..2^40 bytes served to one viewer |

`ps3dk_prof_live_start`: `OK`; `ENOTINIT` before init; `ESTOPPED` after shutdown began; `ENOTSUP` for a callback sink,
a rolling capture, or a capture outside `/dev_hdd0/`; `EALREADY` (once per run); `EINVAL`; `ENET` (no listener);
`ETHREAD`. In an extension build link `libps3dkprof_ext_live.a`. In an AUTO build the app must link
`ps3dkprof_ext_live` and call live start itself (with `PS3DK_PROF=1` so the call is declared); this combination has
not been tested.

`ps3dk_prof_live_stop`: `OK`; `ETIMEDOUT` / `ECLOCK` (still stopping: call again, keep the network up); `ETHREAD`
(terminal; never tear down the network); `EALREADY`; `ENOTINIT`. `ps3dk_prof_live_get_stats(&s)` (EXPERIMENTAL)
copies the agent's final counters after a successful stop.

---

### 5.7 RSX frame capture and draw labels

With `PS3_PROF_RSX_FRAME` (4.12), `<ps3dk/prof_auto.h>`:

```c
int id = PS3DK_PROF_RSX_FRAME_CAPTURE("after-camera-cut");  /* any thread: > 0 the capture id */
PS3DK_PROF_DRAW_SCOPE_BEGIN("terrain");                      /* owner thread */
PS3DK_PROF_DRAW_LABEL("rock_large/lod1");                    /* later draws read terrain/rock_large/lod1 */
draw_rocks();
PS3DK_PROF_DRAW_SCOPE_END();
```

A request is armed at the AUTO owner's next frame boundary when a capture window is open (an app's
`PS3DK_PROF_CAPTURE_WHOLE_RUN()` counts); otherwise the capture opens a two-frame window named `rsx-frame` and arms at
the boundary after. The frame from the arming boundary to the next one is captured. A request that finds no window
after three boundaries is abandoned and nothing is written. Results: the capture id, `PS3DK_PROF_EBUSY` (one
outstanding), `PS3DK_PROF_ENOTSUP` (not selected, no ring, no AUTO owner), `PS3DK_PROF_EINVAL`, `PS3DK_PROF_ENOMEM`
(no staging memory), `PS3DK_PROF_ESTOPPED`; 0 without `PS3_PROF_AUTO`.

Labels are owner-thread only. A label applies until replaced, its scope ends or the frame ends; scopes nest to depth
8; a path is cut at 63 bytes. The strings must stay valid until then; they are copied only into a capture. Outside a
capture each call stores one pointer.

## 6. The PC app (`ps3dk-prof-viewer`)

### 6.1 Install and launch

`ps3dk-prof-viewer` (and `ps3dk-prof`) ship with the host tools: `$PS3DEV/bin` on Linux, the `.exe` files in the
Windows SDK zip. From source: `cargo build --release -p ps3dk-prof-viewer` in `tools/`.

```
ps3dk-prof-viewer [capture.prof | capture.profr]
    [--page=timeline|frames|zones|paths|waits|fifo|auto|rolling]   default: Overview (a .profr opens on Rolling)
    [--view=START:LEN]                 Timeline window, in ticks from the capture start
    [--graph=bars|lines|distribution|map]   Frames graph mode (default bars)
    [--connect=IP:PORT]                start a live session at launch
    [--capture-dir=DIR]                save folder for --connect
    [--light | --dark | --theme=light|dark]
    [--min-size | --width=N --height=N]
    [-v | --version]
```

### 6.2 Opening captures

**Open…** (or drag a file onto the window) accepts `.prof` and `.profr`; the type is decided by the file's content.
A file named `*.incomplete.prof` is read strictly: it is never promoted to a complete capture. The app bar shows the
reader's verdict (`COMPLETE`, `INCOMPLETE`, `MALFORMED`; section 8.2).

### 6.3 Pages

Pages appear in the sidebar when the capture has the data for them.

| Page | Shows | Needs |
|---|---|---|
| **Overview** | Identity, clock, lanes, verdict, WINDOWS list (id, name, kind, frames, end reason); EXTENSION SOURCES and COUNTERS when present | any capture |
| **Timeline** | PPU threads, SPU lanes, RSX domains and AUTO lanes on one axis. Wheel: zoom at the pointer; drag: pan; Shift+drag: zoom to a box; double-click: fit; click a span: highlight its zone. Follow newest data while live. | zones inside a window |
| **Frames** | Per-frame bars against a 30/60 FPS budget (or none); graph modes Bars, Lines (with Raw/Mean 5/15/30 smoothing and Fit p99/Max scale), Distribution, Verdict map; per-lane tables | `PS3DK_PROF_FRAME` or AUTO flips |
| **Zones** | Named PPU zone statistics: instances, completed, censored, total, mean, p50/p90/p99 (refused below 20 samples), max | zones inside a window on MEASUREMENT VALID lanes |
| **Hot paths** | Call paths with the most measured time; with function paths, a **Source: Zones / Functions (automatic)** selector, the per-thread call tree and **Load ELF symbols…** | zones; function paths for the Functions view |
| **Waits** | Wait observations per actor | a waits build; otherwise "UNSUPPORTED" |
| **FIFO wrap waits** | Per-wrap phase bounds on the PPU clock | FIFO or AUTO build |
| **Automatic** | AUTO coverage per frame, reached-marker intervals, clock map, per-frame verdict; **View in Timeline** | AUTO build |
| **Passes** | RSX time per tag and per range | AUTO build with tags or ranges |
| **RSX frames** | RSX frame captures already present in the file | a capture with a GPU source |
| **Snapshot** | A frozen frame (section 6.6) | Capture full frame |
| **Rolling (retained)** | Store state, retained segments, PPU zones, AUTO and FIFO data of a `.profr` | a rolling file |

Times are milliseconds with three decimals; hover for raw ticks. Without a usable clock the app shows ticks rather
than inventing a conversion. Empty pages name the missing records or capability rather than showing zero work. SPU
durations are SPU decrementer ticks; SPU placement with order records uses causal bounds and the RMAX1 premise
(PROVISIONAL until the order is final); legacy captures use the W32/R64 premise toggles on Timeline, Frames and Hot
paths. RSX units read `MEASURED`, `CONDITIONAL` or `UNMEASURED`, each with reasons; reached-marker intervals include
queued work and are not isolated draw cost.

With a rolling file open, the other pages show "Needs a complete capture": they analyze only complete captures.

### 6.4 Connecting live

What the app needs: the profiler linked (section 2), and either live auto-start turned on (section 4.3, no app code) or
`netInitialize` and `ps3dk_prof_live_start` in its code (section 5.6); a FILE sink on `/dev_hdd0/`, and no rolling
output.

Press **Connect…**: console IP address (a literal IP), port (default `31750`), and the folder for the saved copy
(default `Documents/PS3DK Captures` in your home folder). While data arrives the app bar shows `LIVE · PROVISIONAL`;
**Disconnect** ends the session. The viewer accepts at most 256 MiB per session.

When the session ends:

- the console sent END and every check passed: saved as `<session>-<name>.prof`, verdict `COMPLETE`;
- no trailer (Disconnect, a dropped link, the app quit from the XMB) but a valid prefix with at least one event chunk:
  saved as a normal `.prof`, `COMPLETE` with the note "ended without a trailer";
- a protocol, integrity or local write failure: kept as `<name>.incomplete.prof` with the reason, or nothing saved.

### 6.5 Live capture triggers

While a live session runs, a strip under the app bar has a window name field (default `viewer`), **Begin**, **End**,
a frame count (1..1,000,000, default 60) and **Capture N frames**. They open and close capture windows in the running
app without app code: the request is applied in the app's next `ps3dk_prof_frame` and recorded like a window the app
opened. One command is outstanding at a time; each gets an answer:

| Answer | Meaning |
|---|---|
| OK | window opened or closed (its id is shown) |
| `EBUSY` | a window is already open or opening, for example the app's own `CAPTURE_WHOLE_RUN` |
| `EPERM` | End while the open window is the app's |
| `EINVAL` | End with no window, or a bad name (1..63 bytes, no NUL) |
| `EFULL` | no queue room, or 32 distinct names already used this run |
| `ETIMEDOUT` | no frame mark took the command within 5 s (the app is not calling `PS3DK_PROF_FRAME`) |
| `ESTOPPED` | the app's profiler is shutting down |

The buttons are disabled with a reason against an older agent (protocol 1). To drive capture from the PC, build the
app without its own window, or close the app's window first.

### 6.6 Capture full frame, snapshots and bookmarks

**Capture full frame** freezes the frame selected on the Frames page (or the newest measured frame) as a SNAPSHOT of
the data observed so far, on a file or a live session. The Snapshot page draws one row per PPU thread, SPU lane and RSX
domain with solid cores, whiskers for timing uncertainty, possible and extrapolated spans, and a per-lane coverage list
(`OBSERVED`, `PARTIAL`, `NO DATA`, `UNMAPPABLE`). **Re-resolve with W32/R64**, **Delete snapshot** and **Back** are on
the page. Bookmarks are written next to the saved capture as `<name>.frames.txt`. Limits: 64 snapshots per session,
1 GiB accounted memory across sessions.

**Save snapshot** writes an independent `.prof`; **Save as…** chooses its destination. Existing files are never
replaced. Reopening this file selects the frozen frame and its W32/R64 and extrapolation premises, without needing
the original session, console or bookmark file. Saving during live reception uses the frozen bytes; later network
data does not change the saved result. One background save runs at a time.
Save on an already-saved snapshot reports its existing path immediately; Save as
can publish another independent copy without replacing a file.

A saved snapshot contains its raw timing dependencies, including cross-edge zones and children needed for exact
exclusive time, overlapping AUTO operations, and the original clock-map inputs. It also carries the original
source's diagnostic aggregates, explicitly marked as source-wide provenance on reopen. Those counts never create
timing bars. Before publishing, the viewer reopens the projection and compares the resolved snapshot exactly.
Source seal proofs are not carried: their chunk references belong to the original file. When present in the
original, their count is shown as provenance, not as completion proof for the saved snapshot.
The support-file limit is 64 MiB; an oversized dependency is named in the refusal, and the viewer never falls back
to copying the whole session. The default 1 GiB shared budget imposes a lower practical save limit of about
14 MiB of selected support: workspace is charged at 70 times the output size plus 4 MiB, and retained snapshots
reduce the available budget further. Larger saves are refused for workspace even below the 64 MiB file cap.
Only contributors' direct children are needed for
exclusive time, not all grandchildren below a run-long root. The save result
reports the direct-child record bytes. A large direct-child dependency is named
when it exceeds the allowance.

Freezing retains bounded closure inputs from the already-parsed document. The
first Save prepares raw support in its worker and parses only that support,
without reanalysing the whole session. Frozen-input retention has
an 8 MiB per-snapshot allowance and counts against the shared viewer budget. If
preparation cannot fit, the snapshot remains viewable and only Save is unavailable,
with its reason shown. Save preparation reserves at most 64 MiB of scratch; after projection, parsing
and output work is charged from the actual selected support size. Neither
reservation grows with the original session length.

In the Mossbrook beta 4.10 example, frames 1112 and 1134 save as 101,049 and 101,565 bytes from a 2,157,057-byte
capture, including 19,472 bytes of clock support each. Clock anchors are capture-wide and grow with run length;
a sufficiently long capture can exceed the support limit from clock inputs alone. Raw EXT epoch/event inputs
needed to validate an SPU or explicit RSX map are also kept and charged to this limit.
An EXT-heavy test with 4,096 SPU pairs saves 67,062 bytes, including 65,976 bytes of raw EXT support.

The live strip keeps the latest 64 successfully requested windows, including after Disconnect. Each has Save and
Save as controls. An ACK does not prove that its records have arrived: the viewer explains when the window is
absent or has no observed frames in the displayed accepted prefix. An open window saves the frames observed so
far and remains open in the saved file; Save as creates a later revision without overwriting the earlier one.
Window files use the ordinary window exporter described
below; their timing is reconstructed from the records retained in that export.

In AUTO builds the snapshot also has an `rsx.auto` row with the retained draw, CALL and range records, placed through
the reader's qualified AUTO clock map within its interpolation support. Their execution-frame tokens select the frame,
including CALL-segment draws built in an earlier frame. A ranges-only capture shows recorded ranges, not individual
draw bars. Untimed draws and records with unavailable timing stay counts, with reasons; replayed segments or invalid
clock evidence are never drawn. The AUTO-declared RSX domain with no extension events says **timed by AUTO records**,
pointing to `rsx.auto` for clock validity, and a PPU lane holding only frame marks says **no zones in this build**.
These labels do not claim complete coverage or per-draw state.

**Capture RSX frame** in an app built with `PS3_PROF_RSX_FRAME` (4.12): the live link (protocol 3) carries the request
and its progress. Press the button in the live strip: once the reader has the capture and its timing, the viewer
opens **RSX frames**, selects the new capture, and focuses its slowest timed draw. Written progress alone does not
mean Ready: missing AUTO records show **Pending timing**. A truncated or abandoned capture opens as **Partial** with
its reason. A request without a new frame boundary stays waiting; Disconnect keeps the received evidence. Older
agents are negotiated as protocol 2 then 1 and show why the button is unavailable; requests are never retried.

Draws default to descending time, with ordinal ties and unavailable times last. Filter by label, shader, kind or
ordinal, change the sort, or group by program pair, VP, FP, label, tag, pass or kind. Summary and Shaders show the
reader's arguments, provenance and stable identities. State, Textures, Buffers and Constants explicitly report that
schema 1 did not record those values. PIPELINED intervals may overlap: their sum is not isolated execution cost.
Qualified AUTO clocks give millisecond bounds (three decimals), with raw ticks on hover; unqualified clocks retain
raw ticks and their unavailable-conversion notice. Existing EXT-marker captures remain a separate source selector.

**Shader folder…** scans that folder's `.vpo`/`.fpo` files in the background, matching the reader's canonical hash.
Names supplied by the app take precedence over local filenames, then stable hashes. The folder is session-local;
the scan visits at most 4096 directory entries and reads at most 64 MiB, reporting invalid files or a reached limit.

Each RSX capture has **Save** and **Save as…**. These use the accepted file/live prefix and the frame exporter, then
reopen the result and require identical capture data, timing and partial status before publishing. Existing paths
are never overwritten; repeat Save reports the saved path. Saves work after Disconnect when the prefix was accepted.
Other captures from the same frame can be included and remain selectable on reopen. One save worker uses the shared
viewer budget (about 70 times the source bytes plus 4 MiB); insufficient workspace refuses visibly. A sole capture
is selected automatically on reopen. The original session file is untouched.

### 6.7 Windows: picker, Save window, Save range

With a capture open, the app bar has a window picker (**All frames** or `window N — name`). A window scopes the
Passes, Automatic, Frames and Timeline pages to its frames. **Save window…** writes `<name>.window_<id>.prof` and
**Save range…** writes `<name>.<first>..<last>.prof` next to the open file, with the same exporter as
`ps3dk-prof export`. Neither overwrites an existing file: the status line names the file written, or the existing
file that made the save refuse.

In a live session the saves go into the folder chosen in **Connect…** and use the received bytes behind the displayed
document, even if the session's saved file was moved or changed. Only the reader-accepted prefix is exported: a torn
trailing chunk and bytes received after that snapshot are left out. Saving still works after **Disconnect** when an
early prefix was accepted; a session that failed validation refuses the export with a reason.

### 6.8 Rolling captures

Open a `.profr` like any capture (limit 64 MiB). The Rolling page lists the store, retained segments and the data
still present in them. Evicted history is not reconstructed: whole-capture totals are unavailable, and a retained
frame can read UNKNOWN when its build or clock evidence was evicted. `ps3dk-prof rolling` prints the same listing.

### 6.9 Making every feature available to any profiler-linked app

Today several PC app features depend on app code or a specific build:

| Feature | Plain `libps3dkprof.a` | Extension builds | AUTO |
|---|---|---|---|
| Open file, Overview, Capture full frame, picker, Save | yes | yes | yes |
| Timeline, Zones, Hot paths | need zones and a window | same | window; zones optional |
| Frames | needs `PS3DK_PROF_FRAME` | same | from flips |
| Live connect and triggers | `PS3_PROF_LIVE_AUTOSTART=ON`, or `netInitialize` + `ps3dk_prof_live_start` | same + `libps3dkprof_ext_live.a` (added by the option) | `PS3_PROF_LIVE_AUTOSTART=ON`; untested on AUTO |
| SPU/RSX rows | no | needs SPU lane / GPU attach code | RSX automatic |
| Functions view | no | `_func` variants | no |
| Waits | no | `PS3_PROF_WAITS` | no (refused) |
| FIFO page | no | FIFO selector | yes |
| Automatic, Passes | no | no | yes (Passes needs tags or ranges) |
| Rolling | no | no | `PS3_PROF_AUTO_ROLLING`, not live |
| Capture RSX frame | no | no | `PS3_PROF_RSX_FRAME` + ring; live request (protocol 3) or app call |

Work in progress to close these gaps: a default capture window (t_7adaacd4); per-capture timing choices
(t_6dd7fb83); SPU, zone and function switches (t_2873fe00); until-marker windows (t_3ea33a77); finishing a capture
from the PC (t_dd5b7ce1); startup settings (t_9d1f2c10); rolling captures in the viewer (t_149a6545, t_914a084f); and
RSX frame capture requested from the PC app (t_1eec0d29). The [API reference](profiler-api.md#16-parallels-app-api-and-pc-app)
lists each app call next to its PC app equivalent.

### 6.10 Troubleshooting

- **Live never connects.** The PC can open a TCP connection to the console's IP on `31750` (or the configured port);
  the app called `ps3dk_prof_live_start` and it returned `OK` (log its result), or, with auto-start, the TTY shows
  `ps3prof: live viewer: ...` and not `ps3prof: live auto-start off: ...`; the capture is on `/dev_hdd0/`, not a
  callback sink, not rolling. The connect dialog says "The app must be running with the live agent enabled."
- **The capture is empty** (no zones, Zones page "No zones recorded"). No capture window was open: add a window in
  code or press Begin in a live session.
- **Frames page "No FRAME records".** The app does not call `PS3DK_PROF_FRAME()` (non-AUTO builds).
- **Triggers answer `ETIMEDOUT`.** No thread marks frames. **`EBUSY`**: the app keeps its own window open.
- **Waits page "UNSUPPORTED"**, no Automatic or FIFO page: the capture comes from a build without that feature.
- **SPU rows UNMAPPABLE.** Check whether the capture has SPU order records; for legacy captures use W32 only with
  evidence of no decrementer wrap within each epoch.

---

## 7. The `ps3dk-prof` command line

Run `check` before interpreting a capture.

```sh
ps3dk-prof check capture.prof                       # verdict
ps3dk-prof report [--window <id|name>] capture.prof  # verdict and full report
ps3dk-prof windows capture.prof                     # capture windows: id, name, source, kind, frames, end reason
ps3dk-prof frames [--window <id|name>] capture.prof # frame elapsed intervals
ps3dk-prof paths capture.prof                       # PPU hot paths
ps3dk-prof paths --functions [--elf app] [--thread NAME] [--interval N | --all] [--top K] [--json] capture.prof
ps3dk-prof counters [--json] capture.prof           # named per-frame counters
ps3dk-prof waits [--json] capture.prof              # wait observations
ps3dk-prof fifo [--json] capture.prof               # FIFO wrap phases
ps3dk-prof auto [--window <id|name>] [--json] capture.prof   # AUTO coverage, clock map, per-frame verdicts
ps3dk-prof auto --ops [--frame N] [--json] capture.prof      # every AUTO operation record
ps3dk-prof auto --by-tag [--json] capture.prof      # per frame and tag; range time per name
ps3dk-prof rsx-frame [--capture N] [--draw K | --group-by program|vp|fp|label|tag|pass|kind] [--json] capture.prof
ps3dk-prof shader-hash [--json] file.vpo file.fpo   # canonical program hashes, as captures name programs
ps3dk-prof export --window W | --frame N | --frames A..B in.prof out.prof   # standalone sub-capture
ps3dk-prof rolling [--json] [--max-bytes N] [--max-analysis-bytes N] [--max-analysis-work N] capture.profr
ps3dk-prof clockmap [--premise spu-nowrap] capture.prof
ps3dk-prof measured [--xrsx-submit NAME --xrsx-wait NAME] capture.prof
ps3dk-prof spu-ticks [--pairs] [--observations] [--spu-isolation-scale] [--premise spu-nowrap] [--capture-ops N] capture.prof
ps3dk-prof check-arcs file.arcs | arcs-json file.arcs | check-pair file.arcs file.prof
```

Exit status: `0` COMPLETE (also a file without a trailer; its output then has the line `ending: NO-TRAILER`), `1`
INCOMPLETE, `2` MALFORMED, `3` usage or I/O error, `4` COMPLETE but a domain-extension source is not clean (only files
with SPU/RSX extensions). `auto`, `fifo` exit `1` when the evidence is absent or refused.

For `paths --functions --elf`, pass the unstripped linked ELF (in CMake, the link target's own output with no `.elf`
suffix), not the stripped `.elf` placed beside the SELF. A build-id mismatch prints a WARNING.

---

## 8. Reference

### 8.1 File formats

- **`.prof` (format v1).** A 16-byte file header, then checksummed chunks: HEADER, CLOCKS, optional PROFILE, event
  chunks, extension chunks selected by file flags, and a TRAILER. One file holds every capture window of a run.
  [ps3dk-prof-format.md](design/ps3dk-prof-format.md), allocations in
  [ps3dk-prof-format-allocations.md](design/ps3dk-prof-format-allocations.md).
- **`.profr` (rolling).** A fixed-size store of retained segments and checkpoints written by AUTO rolling output.
  [ps3dk-prof-rolling-runtime.md](design/ps3dk-prof-rolling-runtime.md),
  [ps3dk-prof-rolling-store.md](design/ps3dk-prof-rolling-store.md).
- **Live protocol (v1, v2 with capture triggers, v3 with the RSX frame request).** [ps3dk-prof-viewer.md](design/ps3dk-prof-viewer.md); schema in
  `sdk/libps3dkprof/src/live/live_proto.h`.
- **AUTO records.** [ps3dk-prof-auto-rsx.md](design/ps3dk-prof-auto-rsx.md),
  [ps3dk-prof-auto-lifecycle.md](design/ps3dk-prof-auto-lifecycle.md).
- **Function paths, waits, FIFO.** [ps3dk-prof-func-paths.md](design/ps3dk-prof-func-paths.md),
  [ps3dk-prof-waits.md](design/ps3dk-prof-waits.md), [ps3dk-prof-fifo.md](design/ps3dk-prof-fifo.md).
- **Arc companion (`.arcs`).** [ps3dk-prof-arcs.md](design/ps3dk-prof-arcs.md).
- **Bookmarks (`<name>.frames.txt`).** Written by the PC app beside a saved capture; reopening one re-reads the whole
  capture and reports a changed or refused file.
- **RSX frame captures (file_flags bit 10, chunks 44..48).** Format section 12 of
  [ps3dk-prof-format.md](design/ps3dk-prof-format.md).

### 8.2 Verdicts

| Verdict | Meaning |
|---|---|
| `COMPLETE` | Structurally complete, header to trailer. Whether the run finalized cleanly is the result of `ps3dk_prof_shutdown` on the console. |
| `COMPLETE (ended without a trailer)` | Valid chunks and at least one event chunk, no trailer (app quit, link ended, copy cut short). Exit `0`, line `ending: NO-TRAILER`. A torn last chunk is dropped. |
| `INCOMPLETE` | The trailer reports an unclean run (`NOT_QUIESCED`, `SEQ_LIMIT`, sink errors), or there is no event chunk ("no events recorded (ended before the first drain)"). |
| `MALFORMED` | A bad checksum, length, structure or count in the bytes present. |

### 8.3 Result codes (`<ps3dk/prof.h>`)

| Code | Value | Meaning |
|---|---|---|
| `PS3DK_PROF_OK` | 0 | |
| `EPERM` | -1 | not the drain (init) thread in APP mode; not the window's thread |
| `EBUSY` | -2 | inside a drain, a drain running, init or a setter running; a window open |
| `EALREADY` | -3 | init, shutdown, thread begin or live start already done |
| `EEXIST` | -4 | the capture file exists |
| `EINVAL` | -5 | a configuration or argument out of range |
| `ENOMEM` | -6 | |
| `ENOTQUIESCED` | -7 | shutdown: recording threads did not quiesce |
| `ESEQLIMIT` | -8 | a sequence or counter ceiling was reached |
| `ESINKRECOVERED` | -9 | sink errors were recovered |
| `ESINKFAIL` | -10 | the sink failed; no trailer |
| `ECLOSE` | -11 | close failed after the trailer: the run fails |
| `ETHREAD` | -12 | thread identity, creation or join failed |
| `ECLOCK` | -13 | timebase or monotonic clock unavailable |
| `ENOTINIT` | -14 | not initialized |
| `ESTOPPED` | -15 | shutdown has begun |
| `EFULL` | -16 | no thread slot; window queue or name table full |
| `ENOTSUP` | -17 | callback sink, unmeasured device, rolling with live, off build |
| `ENET` | -18 | live listener could not be created |
| `ETIMEDOUT` | -19 | live stop not finished; call again |
| `ERETENTION` | -32 | capture closed; retention bookkeeping failed |
| `ERESOURCE` | -33 | rolling stopped at a configured bound |

The SPU lane writer uses -20..-23 (`EAGAIN`, `EFAULT`, `EEXHAUSTED`, `EDEPTH`; `prof_spu_lane.h`).

### 8.4 Known limits

- Non-DEX consoles need the profiler linked; DEX support without linking is planned.
- The live link needs app code, a FILE sink on `/dev_hdd0/`, and no rolling output; one viewer at a time.
- Rolling output is AUTO-only and refuses active SPU lanes.
- AUTO cannot be combined with waits, FIFO selection or another profiler archive; LP64 AUTO is not run on hardware.
- RSX frame capture (beta): one capture at a time; no render state, textures, buffers or constants yet; draws inside
  CALL segments are timed by AUTO but not listed; a fragment program bound without a program container is recorded by
  address only. Times are reached-marker intervals (they include queued work and overlap neighbours).
- `quick_exit` makes one best-effort drain of PPU zones and frames only (no trailer, no extension data).
- SPU durations are decrementer ticks, not wall time; SPU zones in SPURS jobs are untested.

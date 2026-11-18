# Profiler API reference

This is the app-side API of the PS3DK profiler: every public function, macro, type, field and constant in the
profiler headers, grouped by area. For setup, build options and the PC app, see the
[profiler user guide](profiler-guide.md).

Headers covered (installed under `$PS3DK/ppu/include/ps3dk/` unless noted):

| Header | Area | Runtime |
|---|---|---|
| `prof.h` | core: config, init/shutdown, threads, frames, zones, capture windows, live, results | every profiler archive |
| `prof_spu.h`, `psp_desc.h` | SPU lanes, PPU side | extension archives (`libps3dkprof_ext*.a`, `libps3dkprof_auto.a`) |
| `spu/include/ps3dk/prof_spu_lane.h` | SPU lanes, SPU side | `spu/lib/libps3dkprof_spu.a` |
| `prof_gpu.h` | RSX domains (app GPU zones) | extension archives |
| `prof_auto.h` | AUTO RSX observation: tags, ranges, options, seal | `libps3dkprof_auto.a` (`PS3_PROF_AUTO=ON`) |
| `prof_rolling.h` | rolling output options and result | `libps3dkprof_auto.a` |
| `prof_counter.h` | named counters | `libps3dkprof_counters.a`, `libps3dkprof_ext_counters.a` |
| `prof_func.h` | function-path limits | `libps3dkprof_func.a`, `libps3dkprof_ext_func.a` |
| `prof_wait.h` | wait-observation limits | `libps3dkprof_wait.a`, `libps3prof_wait.a`, `libps3dkprof_ext_wait.a` |
| `prof_fifo.h` | FIFO wrap observation | `libps3dkprof_ext_fifo.a` + `libgcm_cmd_prof_fifo.a` |

`prof_wait_internal.h` (and the SPU-side `psp_spu.h`, `psp_layout.h`) are private ABI used by the observed libraries
and the SPU writer; applications do not call them.

## Conventions

- **The `PS3DK_PROF` switch.** Compile with `-DPS3DK_PROF=1` to enable the API. With `PS3DK_PROF` undefined or `0`
  ("off build") the macros compile to nothing that runs: their arguments are not evaluated (they appear only inside
  `sizeof`), no profiler function is referenced, and each macro yields the value given in its entry (`OK`, `ENOTSUP`
  or an INERT handle). Types and constants are declared in both builds. Prefer the macros in code that must also
  build without the profiler; the functions are declared only in an enabled build.
- **AUTO units.** The `PS3_PROF_AUTO=ON` selector defines `PS3TC_PROF_AUTO=1` for every C/C++ unit. Capture-window
  macros and `prof_auto.h`/`prof_rolling.h` macros are live in such units even without `PS3DK_PROF`.
- **Results** are `int` codes from `prof.h` (section 15). "Before init" means before a successful `ps3dk_prof_init`.
- **Caller-serialized**: `ps3dk_prof_init`, `ps3dk_prof_live_start`, `ps3dk_prof_shutdown` and `ps3dk_prof_live_stop`
  must never run concurrently with each other.
- **PC app equivalent** names the `ps3dk-prof-viewer` action that does the same thing, or `none yet` with the card
  that tracks it.

---

## 1. Configuration, init and shutdown (`prof.h`)

### `ps3dk_prof_config`

```c
typedef struct ps3dk_prof_config {
    uint32_t mode;                /* PS3DK_PROF_MODE_APP (1) or PS3DK_PROF_MODE_THREAD (2) */
    uint32_t max_threads;         /* 1..1024 */
    uint32_t max_descriptors;     /* 1..65536 */
    uint32_t ring_records;        /* power of two, 512..1048576 */
    uint32_t drain_interval_us;   /* THREAD mode: 1000..1000000 */
    uint32_t drain_stack_bytes;   /* THREAD mode: 16 KiB..1 MiB */
    uint64_t quiesce_timeout_us;  /* 0..60000000 */
    uint32_t auto_register;       /* 0: zones from unregistered threads are ignored */
    uint32_t reserved;            /* 0 */
    const char *app;              /* 1..32 of [A-Za-z0-9_-]; names the capture file */
    const char *build_id;
    const char *flags;
    const char *workload_tag;
} ps3dk_prof_config;
```

Defaults from `ps3dk_prof_config_defaults`: APP mode, 64 threads, 1024 descriptors, 4096 records, 10000 us, 65536
bytes, 1000000 us, `auto_register` 0, `app` `"app"`, the strings NULL.

`PS3DK_PROF_MODE_APP` (1u): the app drains (`ps3dk_prof_drain`) on the thread that called init.
`PS3DK_PROF_MODE_THREAD` (2u): an internal thread drains every `drain_interval_us`.

### `void ps3dk_prof_config_defaults(ps3dk_prof_config *cfg)`

Fills `cfg` with the defaults above (NULL is ignored). Any time, any thread. Archive: any.
PC app equivalent: none yet (startup knobs, card t_9d1f2c10).

### `int ps3dk_prof_set_sink_path(const char *path)`

Names the capture DIRECTORY (1..255 bytes). The file is `<dir>/<app>-<16 hex digits of the init timebase>.prof`,
created exclusively. Default directory `/dev_hdd0/tmp`.
Returns `OK`; `EINVAL` (bad length); `EBUSY` while a setter or init runs; `EALREADY` after init. Init returns
`EEXIST` if the file exists and `EINVAL` if the full name exceeds 255 bytes. Before init only; the last successful
setter wins.
PC app equivalent: none yet (startup knobs, card t_9d1f2c10). The Connect dialog's folder sets only where the PC
copy is saved.

### `int ps3dk_prof_set_sink_callbacks(const ps3dk_prof_sink_callbacks *cb)`

```c
typedef struct ps3dk_prof_sink_callbacks {
    long (*write)(void *ctx, const void *buf, unsigned long n);  /* accepted bytes in [0, n], or negative */
    int (*flush)(void *ctx);                                     /* optional: once, after the trailer */
    int (*close)(void *ctx);                                     /* once; negative fails the run */
    void *ctx;
} ps3dk_prof_sink_callbacks;
```

Replaces the file with caller callbacks. Callbacks run inside a drain; a blocking callback is not bounded by
`quiesce_timeout_us`. With a callback sink there is no capture record, no live link, no retention and no rolling
output (`ENOTSUP`). Before init only; same gate results as `set_sink_path`.
PC app equivalent: none.

### `int ps3dk_prof_set_capture_retention(uint32_t keep_completed)`

0 (default) disables; 1..1024 keeps that many completed captures of this exact `app` in the capture directory,
deleting older indexed ones at successful shutdown. FILE sinks only. Before init only (`EBUSY`/`EALREADY` as above,
`EINVAL` out of range). Shutdown returns `ERETENTION` when the capture closed but cleanup failed. In AUTO builds the
profiler initializes before `main`, so this setter is too late there.
PC app equivalent: none yet (startup knobs, card t_9d1f2c10).

### `int ps3dk_prof_init(const ps3dk_prof_config *cfg)` / `PS3DK_PROF_INIT(cfg)`

Creates the capture and publishes it. Returns `OK`; `EBUSY` while a setter or init runs; `EALREADY` after a
successful (or terminally failed) init; `EINVAL` for a bad config; `EEXIST` (file exists); `ENOMEM`; `ECLOCK`;
`ETHREAD`; `ESINKFAIL`; `ENOTSUP` (a pre-init option this archive or sink does not support). A failed
init publishes nothing and every later profiler call stays harmless. The thread that calls it becomes the APP-mode
drain thread. Caller-serialized. Off build: `PS3DK_PROF_INIT(cfg)` is `OK` and does nothing.
AUTO builds and `libps3prof.a` call init themselves before `main` (THREAD mode, `auto_register` 1).
PC app equivalent: none yet (live auto-start, card t_276ffda7; startup knobs, card t_9d1f2c10).

```c
ps3dk_prof_config cfg;
ps3dk_prof_config_defaults(&cfg);
cfg.app = "my-app";
cfg.mode = PS3DK_PROF_MODE_THREAD;
if (PS3DK_PROF_INIT(&cfg) != PS3DK_PROF_OK) { /* run unprofiled */ }
```

### `int ps3dk_prof_shutdown(void)` / `PS3DK_PROF_SHUTDOWN()`

Sets the stop latch, waits up to `quiesce_timeout_us` for recording threads, drains, closes an open window (reason
SHUTDOWN), writes the trailer, calls `flush` and `close`, and returns the run's result: `OK`; `ENOTQUIESCED`;
`ESEQLIMIT`; `ESINKRECOVERED`; `ESINKFAIL` (sink failure or no trailer); `ECLOSE` (close failed: the run fails);
`ECLOCK`; `ETHREAD` (THREAD mode: the drainer join failed on an otherwise OK run); `ERETENTION` (only on an otherwise
OK run with retention set); `ERESOURCE` (rolling output stopped at a configured bound, unless a sink or close failure
replaces it); `ENOTINIT`; `EALREADY` (second call, also after `quick_exit` claimed shutdown); `EPERM` (APP mode, not
the init thread); `EBUSY` (called from inside the drainer, e.g. a sink callback).
Caller-serialized. Off build: `OK`. For a rolling capture the same value is published in
`ps3dk_prof_get_rolling_result().result` (section 13).
PC app equivalent: none yet (finish capture, card t_dd5b7ce1). Disconnect only ends the PC session.

### `int ps3dk_prof_get_capture_record(ps3dk_prof_capture_record *out)` / `PS3DK_PROF_GET_CAPTURE_RECORD(out)`

```c
typedef struct ps3dk_prof_capture_record {
    char path[256];          /* the file the FILE sink created, NUL-terminated */
    uint64_t session_id;     /* the timebase at init (also in the file name) */
    char app[33];
    uint32_t app_len;
    uint8_t build_id[512];   /* build_id_len bytes, not NUL-terminated */
    uint32_t build_id_len;
} ps3dk_prof_capture_record;
```

Returns `OK`; `EINVAL` (NULL); `ENOTINIT` before a successful init; `ENOTSUP` with a callback sink. Any thread after
init; the record never changes. Off build: `ENOTSUP`.
PC app equivalent: the live session shows the app, session and build id from the agent's HELLO; the status bar shows
the opened file.

---

## 2. Threads (`prof.h`)

### `int ps3dk_prof_thread_begin(const char *name)` / `PS3DK_PROF_THREAD_BEGIN(name)`

Registers the calling thread (a slot out of `max_threads`). Call before the thread's first zone, unless
`auto_register` is 1. Returns, in check order: `ENOTINIT` (not initialized); `EALREADY` (this thread is already
registered); `ESTOPPED` (this thread already acknowledged a stop, or shutdown has begun); `ETHREAD` (the thread lock
or the OS thread id could not be obtained); `EFULL` (no free slot, or incarnation ids exhausted; counted as an
unregistered refusal); `OK`. A thread that called `ps3dk_prof_thread_end` may register again. Wait-observation archives register the init
thread as `init` automatically. Off build: `OK`.
PC app equivalent: none.

### `void ps3dk_prof_thread_end(void)` / `PS3DK_PROF_THREAD_END()`

Releases the calling thread's registration. Call before the thread exits. Harmless on an unregistered thread.
PC app equivalent: none.

---

## 3. Frames (`prof.h`)

### `void ps3dk_prof_frame(void)` / `PS3DK_PROF_FRAME()`

Ends the current frame and starts the next on the calling (registered) thread. It is also where pending capture-window
changes and live trigger requests are applied. In AUTO builds the app's flips call it; an explicit call takes
precedence. Off build: nothing.
PC app equivalent: none (frames always come from the app). Live triggers need this call to be made.

### `ps3dk_prof_token ps3dk_prof_frame_token(void)` / `PS3DK_PROF_FRAME_TOKEN()`

```c
typedef struct ps3dk_prof_token { uint32_t owner; uint32_t reserved; uint64_t frame; } ps3dk_prof_token;
#define PS3DK_PROF_TOKEN_NONE   /* {0, 0, 0}: owner 0 is NONE */
```

Returns the calling thread's current frame token, or NONE. Used by the `_FOR` zone variants,
`ps3dk_prof_gpu_frame` and `ps3dk_prof_auto_frame_end`. Off build: `PS3DK_PROF_TOKEN_NONE`.
PC app equivalent: none.

---

## 4. Zones (`prof.h`)

### Types

```c
typedef struct ps3dk_prof_desc {          /* one per call site, a function-local static made by the macros */
    const char *name, *logical_tag, *file;
    uint32_t line;
    uint32_t scope;                       /* 1 cpu, 2 gpu-complete */
    uint32_t state;                       /* runtime-owned registration state */
    uint32_t reserved;
} ps3dk_prof_desc;

typedef struct ps3dk_prof_zone {          /* zero-initialized = INERT */
    uint32_t kind;                        /* 0 INERT, 1 PUSHED, 2 REFUSED, 3 SPENT */
    uint32_t incarnation, generation, reserved;
    uint64_t id, prev_refused_top, span_top_at_begin;
} ps3dk_prof_zone;
#define PS3DK_PROF_INERT_ZONE             /* a zero zone */
```

Descriptor strings must outlive the process (string literals and `__FILE__` do).

### `ps3dk_prof_zone ps3dk_prof_zone_begin(ps3dk_prof_desc *desc, ps3dk_prof_token token)`
### `void ps3dk_prof_zone_end(ps3dk_prof_zone *zone)`

Begin returns a PUSHED handle, or INERT when the profiler is not ready, the thread is unregistered, shutdown began or
**no capture window is open**; a refused begin returns REFUSED. End completes a PUSHED zone; INERT does nothing.
Fast path (registered descriptor, registered thread): no lock, no allocation. Use the macros below.

### `PS3DK_PROF_ZONE_BEGIN(name)` / `PS3DK_PROF_ZONE_BEGIN_FOR(name, tok)` / `PS3DK_PROF_ZONE_END(h)`

`name` is a string literal. `_FOR` ties the zone to a frame token. Off build: an INERT handle; END does nothing.

### `PS3DK_PROF_ZONE(name)` / `PS3DK_PROF_ZONE_FOR(name, tok)`

Scoped zone ending at the end of the enclosing block: C uses a cleanup attribute, C++ the non-copyable,
non-movable `ps3dk_prof::zone_guard(ps3dk_prof_desc *d, ps3dk_prof_token t)`.

```c
void update(void)
{
    PS3DK_PROF_ZONE("update");
    ps3dk_prof_zone h = PS3DK_PROF_ZONE_BEGIN("physics");
    physics();
    PS3DK_PROF_ZONE_END(h);
}
```

PC app equivalent: none yet; switching zone recording on or off from the PC is card t_2873fe00. The PC app's
Begin/End controls decide when zones are recorded (section 5).

---

## 5. Capture windows (`prof.h`)

Live in `PS3DK_PROF` and `PS3_PROF_AUTO` units; inert otherwise. Nothing heavy is recorded outside a window.

```c
#define PS3DK_PROF_WINDOW_MANUAL  1u  /* CAPTURE_BEGIN/END: opens at the next frame boundary, closes after the current frame */
#define PS3DK_PROF_WINDOW_FRAMES  2u  /* CAPTURE_FRAMES(name, n): closes itself after n frames (reason COUNT) */
#define PS3DK_PROF_WINDOW_SECTION 3u  /* SECTION(name): begins and ends at its call sites */
#define PS3DK_PROF_WINDOW_WHOLE   4u  /* CAPTURE_WHOLE_RUN(): unnamed, open from the call to shutdown */

typedef struct ps3dk_prof_capture_info {
    uint32_t state;        /* 0 none, 1 opens at next boundary, 2 open, 3 closes at next boundary */
    uint32_t id;           /* current or last window id; 0 before the first */
    uint32_t kind;         /* PS3DK_PROF_WINDOW_* */
    uint32_t windows;      /* windows opened so far */
    uint32_t refused;      /* BEGINs refused (EBUSY or EFULL) */
    uint32_t queue_full;   /* of them EFULL */
    uint64_t first_frame;  /* the open window's first frame */
    uint64_t frames;       /* frames recorded in the open window so far */
    const char *name;      /* NULL before the first */
} ps3dk_prof_capture_info;
```

One window at a time. A window belongs to the thread that marks frames, whichever thread begins it; one begun before
any frame mark is adopted by the first thread that marks one.

### `int ps3dk_prof_capture_begin(ps3dk_prof_desc *name, uint32_t kind, uint32_t frames)`

Returns `OK`; `EBUSY` (counted) while a window is open or about to open, or for a SECTION in the frame the previous
window ended in; `EFULL` (counted, nothing opens) when the drainer is too far behind to record the window's BEGIN and
END; `EINVAL` (bad kind, FRAMES with 0, NULL name except WHOLE); `ENOTINIT`; `ESTOPPED`. Use the macros.

### `int ps3dk_prof_capture_end(void)`

Closes the window (MANUAL and FRAMES after the current frame, SECTION and WHOLE now; a window not yet open is
cancelled). From the owning frame thread or the thread that began it. Returns `OK`; `EINVAL` (no window); `EPERM`
(another thread).

### `int ps3dk_prof_capture_state(ps3dk_prof_capture_info *out)` / `PS3DK_PROF_CAPTURE_STATE(out)`

Copies the window slot. Any thread. Off build: `ENOTSUP`.
PC app equivalent: each trigger answer shows the window id; Overview lists WINDOWS of an opened capture.

### `int ps3dk_prof_capture_seal(void)`

`ps3dk_prof_shutdown` with an open window closed with reason SEAL. Same results as shutdown.
PC app equivalent: none yet (finish capture, card t_dd5b7ce1).

### Macros

| Macro | Off-build value | PC app equivalent |
|---|---|---|
| `PS3DK_PROF_CAPTURE_BEGIN(name)` | `OK` | **Begin** in the live trigger strip |
| `PS3DK_PROF_CAPTURE_END()` | `OK` | **End** in the live trigger strip (closes only a window the PC opened) |
| `PS3DK_PROF_CAPTURE_FRAMES(name, n)` | `OK` | **Capture N frames** in the live trigger strip |
| `PS3DK_PROF_CAPTURE_WHOLE_RUN()` | `OK` | none yet (default capture window, card t_7adaacd4) |
| `PS3DK_PROF_SECTION(name)` (C cleanup / C++ `ps3dk_prof::section_guard`, `status()` returns the begin result) | nothing | none yet (until-marker windows, card t_3ea33a77); Begin/End by hand |
| `PS3DK_PROF_CAPTURE_STATE(out)` | `ENOTSUP` | trigger answers, Overview WINDOWS |

```c
PS3DK_PROF_CAPTURE_FRAMES("boss-intro", 120);
{
    PS3DK_PROF_SECTION("load");
    load_level();
}
```

Window names are string literals. The PC app's triggers need the live link (section 12) and a thread calling
`ps3dk_prof_frame`; an app window blocks them (`EBUSY`).

---

## 6. SPU lanes (`prof_spu.h`, `psp_desc.h`, `prof_spu_lane.h`)

Archive: an extension archive linked instead of `libps3dkprof.a`; SPU program linked with `libps3dkprof_spu.a`.
PC app equivalent for this whole area: none yet (SPU on/off switch, card t_2873fe00). SPU lanes appear on the
Timeline, Frames, Hot paths and Snapshot pages.

### PPU side

```c
typedef struct ps3dk_prof_ext_config {
    uint32_t size;                 /* = PS3DK_PROF_EXT_CONFIG_SIZE (40), else EINVAL */
    uint32_t spu_lanes_max;        /* 0 disables lanes (then the next two must be 0); 1..64 */
    uint32_t spu_ring_slots;       /* power of two 64..1048576; 0 = 8192 */
    uint32_t spu_image_bytes_max;  /* 4096..16777216; 0 = 65536 */
    uint64_t spu_mem_max;          /* cap on the extension's share of the init block; 0 = 16 MiB */
    uint32_t gpu_domains_max;      /* 0 disables GPU domains (then gpu_report_slots_max must be 0); 1..4 */
    uint32_t gpu_report_slots_max; /* 0 = 256, else 2..1024 */
    uint32_t reserved[2];          /* 0 */
} ps3dk_prof_ext_config;

struct ps3dk_prof_spu_desc { uint32_t ls_addr, line; const char *name, *file; };   /* psp_desc.h */

typedef struct ps3dk_prof_spu_lane {
    uint32_t handle;               /* lane id 1..1024, never reused */
    uint32_t ring_slots;
    uint64_t ea_ring, ea_ctl;      /* pass these to the SPU program */
} ps3dk_prof_spu_lane;

typedef struct ps3dk_prof_spu_lane_stats {
    uint32_t final_written, fin_state;               /* as stated in the file */
    uint64_t next_seq, events, lost;
    uint32_t have_post_join, post_state;             /* producer counters, not in the file */
    uint64_t post_next_seq, post_older_lost, post_refused;
    uint64_t stale, cancelled, unstable;
} ps3dk_prof_spu_lane_stats;
```

Common results for every lane call: `ENOTINIT` (not initialized; it takes priority over argument errors);
`ENOTSUP` (initialized without lanes); `EBUSY` (inside a drain, sink callback or GPU call, or the extension lock
could not be taken); `ESTOPPED` (a terminal extension fault, the shutdown claim or the seal, or the closing unlock
failed). When only the closing unlock fails, `register_*` and `lane_create` still write `*image` / `*out` and
`lane_end` still writes `*final_or_null`: that output is diagnostic, not permission to use the lane.

| Function / macro | What it does | Results |
|---|---|---|
| `int ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const ps3dk_prof_ext_config *ext)` / `PS3DK_PROF_INIT_EX` | `ps3dk_prof_init` with extensions; `ext` NULL or disabled is exactly `ps3dk_prof_init` | as init, plus `EINVAL` (bad `ext`: size, reserved, ranges, a disabled part with nonzero fields, or the extension exceeding `spu_mem_max`), `ENOTSUP` (rolling output with an unsupported extension combination), `ENOMEM` (block allocation or extension mutex init) |
| `int ps3dk_prof_spu_image_register_table(const uint8_t *build_id, uint32_t build_len, const ps3dk_prof_spu_desc *descs, uint32_t n, uint32_t *image)` / `PS3DK_PROF_SPU_IMAGE_REGISTER_TABLE` | copies a descriptor table for one SPU image | `EINVAL` (NULL argument; `build_len` not 1..64; `n` not 1..4096; a name or file that is not ASCII up to 512 bytes; an entry over the chunk size; the table changed during the copy); `ESTOPPED` (shutdown or seal); `EFULL` (32 images); `ENOMEM` (image arena full); `OK` |
| `int ps3dk_prof_spu_image_register_elf(const void *elf, uint64_t size, uint32_t *image)` / `PS3DK_PROF_SPU_IMAGE_REGISTER_ELF` | registers from the SPU ELF bytes' `ps3dkprof_spudesc` section; the ELF may be freed afterwards | `EINVAL` (NULL argument or any ELF rule), else as table |
| `int ps3dk_prof_spu_lane_create(const char *name, uint32_t image, int owner_self, ps3dk_prof_spu_lane *out)` / `PS3DK_PROF_SPU_LANE_CREATE` | creates a lane; `owner_self` 1 binds it to the calling thread | `EINVAL` (NULL `out`; `owner_self` not 0/1; `owner_self` 1 on an unregistered thread; unknown image; name not ASCII 1..31); `ESTOPPED` (shutdown or seal); `EFULL` (lifetime, live, epoch or loss budget, or no free lane entry); `ENOMEM` (`spu_mem_max` reached: unended lanes stay charged); `OK` |
| `int ps3dk_prof_spu_lane_end(uint32_t handle, uint64_t timeout_us, ps3dk_prof_spu_lane_stats *final_or_null)` / `PS3DK_PROF_SPU_LANE_END` | retires a lane once its producer can never touch it again (SPU group joined, or job fini plus a completion receipt) | works after the seal. `OK` (released); `ESTOPPED` (the lane was finalized by the seal, or an unlock failed); `ETIMEDOUT` (lane still charged: call again); `ECLOCK`; `EINVAL` (stale or already ended handle) |
| `int ps3dk_prof_spu_lane_get_stats(uint32_t handle, ps3dk_prof_spu_lane_stats *st)` / `PS3DK_PROF_SPU_LANE_STATS` | copies lane counters; `*st` written only on `OK` | `EINVAL` (NULL `st`, stale handle); `OK` |
| `int ps3dk_prof_spu_poll_calibrations(void)` / `PS3DK_PROF_SPU_POLL_CALIBRATIONS()` | observes published calibration answers between drains (APP mode helper) | never waits. `EBUSY` (lock contention, or inside a drain or GPU call); `ESTOPPED` (shutdown, fault, seal); `OK` |

Off build: every macro returns `ENOTSUP`. Descriptor capacity: 16 bytes per descriptor in ILP32, 24 in LP64, plus
strings.

### SPU side (`prof_spu_lane.h`, SPU builds only)

```c
typedef struct ps3dk_prof_spu_writer { /* opaque */ uint32_t desc_count, bad_index; } ps3dk_prof_spu_writer;
#define PS3DK_PROF_SPU_OWN_DEC 1u                     /* attach flag: the writer may write the decrementer */
#define PS3DK_PROF_SPU_STAGING_BYTES(ls_cap)          /* ((ls_cap + 8) * 8) */
#define PS3DK_PROF_SPU_WRITER_ALIGN 128u
```

| Function / macro | What it does |
|---|---|
| `int ps3dk_prof_spu_attach(ps3dk_prof_spu_writer *w, uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl, uint32_t tag, void *staging, uint32_t ls_cap, uint32_t flags)` / `PS3DK_PROF_SPU_ATTACH(...)` | attaches a caller-owned writer (128-aligned) and staging buffer (16-aligned) to the lane the PPU created. `ls_cap` even, 4..4096; DMA tag 0..31 |
| `void ps3dk_prof_spu_begin(w, uint32_t desc_index)`, `ps3dk_prof_spu_end`, `ps3dk_prof_spu_mark` | raw events by descriptor index |
| `PS3DK_PROF_SPU_ZONE_BEGIN(w, "name")` -> index, `PS3DK_PROF_SPU_ZONE_END(w, z)`, `PS3DK_PROF_SPU_MARK(w, "name")` | zones; each use defines a 16-byte record in section `ps3dkprof_spudesc` |
| `int ps3dk_prof_spu_flush(w)` / `PS3DK_PROF_SPU_FLUSH(w)` | publishes staged events (nothing flushes on its own) |
| `int ps3dk_prof_spu_fini(w)` / `PS3DK_PROF_SPU_FINI(w)` | final flush; closes the writer |
| `int ps3dk_prof_spu_dec_write(w, uint32_t v)` / `PS3DK_PROF_SPU_DEC_WRITE(w, v)` | writes the decrementer (only with `OWN_DEC`, outside zones) |

SPU result codes: `PS3DK_PROF_SPU_OK` 0, `_EBUSY` -2, `_EINVAL` -5, `_ECLOSED` -15, `_ENOTSUP` -17, `_EAGAIN` -20,
`_EFAULT` -21, `_EEXHAUSTED` -22, `_EDEPTH` -23. Per call:

| Call | Results |
|---|---|
| attach | `_EINVAL` (NULL or misaligned writer/staging, tag > 31, `ls_cap` odd or not 4..4096, `ring_slots` not a power of two in 8..2^20, unaligned EAs, unknown flags, staging overlapping the writer, a bad descriptor section); `_EBUSY` (a blocking event is enabled: retry on the same writer); `_ECLOSED` (the lane already had a producer); `_EFAULT` (an MFC status was wrong); `_OK` |
| flush | `_EFAULT` (a fault latched earlier, including a bad descriptor index, or an MFC or ring check failed); `_ECLOSED` (not attached, or closed); `_EBUSY` (a blocking event is enabled); `_EEXHAUSTED` (generation/write counters spent); `_OK` (published, or nothing to publish) |
| fini | `_ECLOSED` (not attached, or already closed); `_EBUSY`; `_EFAULT`; `_OK`. Never `_EEXHAUSTED`: an exhausted writer still closes |
| dec_write | `_EDEPTH` (a zone open or a BEGIN suppressed; checked first, not cured by flushing); `_ENOTSUP` (no `OWN_DEC`); `_EBUSY` / `_ECLOSED` / `_EFAULT` (from its flush or preflight); `_EAGAIN` (earlier events unpublished, ring nearly full, or counters spent: flush and retry); `_OK`. An `_EFAULT` after the write leaves the decrementer written |
| begin / end / mark | void; an index outside the attached descriptor count records nothing, counts `bad_index` and latches `_EFAULT` | One writer per SPU thread, not reentrant. Off build: the
writer is a 1-byte struct, zone begin yields 0, and the other macros return `_ENOTSUP`.

```c
/* SPU program */
static ps3dk_prof_spu_writer w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t staging[PS3DK_PROF_SPU_STAGING_BYTES(64)] __attribute__((aligned(16)));
PS3DK_PROF_SPU_ATTACH(&w, ea_ring, ring_slots, ea_ctl, 5, staging, 64, 0);
uint32_t z = PS3DK_PROF_SPU_ZONE_BEGIN(&w, "job");
do_job();
PS3DK_PROF_SPU_ZONE_END(&w, z);
PS3DK_PROF_SPU_FINI(&w);
```

Durations are SPU decrementer ticks. Reference lifecycle: `samples/profiling/heterogeneous-workload`.

---

## 7. RSX domains: app GPU zones (`prof_gpu.h`)

Archive: an extension archive, with `ext.gpu_domains_max` set at `ps3dk_prof_init_ex`. Every call on a domain except
attach must come from the thread that attached it (the GCM context owner, a registered thread), with the same
context. Common results: `EBUSY` inside a drain, sink callback or another GPU call; `ENOTINIT`; `ENOTSUP` (no GPU
domains); `ESTOPPED` (fault, shutdown claim, stopped domain); `EPERM` (another thread); `EINVAL` (other context, stale
handle); `ETHREAD` (unregistered thread for begin/mark).

```c
typedef struct _gcmCtxData ps3dk_prof_gcm_ctx;   /* CellGcmContextData / gcmContextData */
typedef struct ps3dk_prof_gpu_zone { uint32_t gpu, depth; uint64_t begin; uint32_t desc, pad; } ps3dk_prof_gpu_zone;
#define PS3DK_PROF_GPU_ZONE_INIT {0u, 0u, 0u, 0u, 0u}   /* INERT; ending it is EINVAL */
typedef struct ps3dk_prof_gpu_stats {
    uint64_t refused_api, refused_mark_depth, refused_suppress_overflow, refused_scope, refused_ctx, owner_busy;
    uint64_t lost_ring, lost_fifo, lost_records, brackets, cal_bad;
    uint64_t events, lost, held, attribution_orphaned, brackets_dropped;
    uint32_t retired, final_written, final_state, fault, anomaly, loss_exhausted, overflow, pad;
} ps3dk_prof_gpu_stats;
```

Internal faults and anomalies are reported as `ESTOPPED`; sequence or depth overflow as `ESEQLIMIT`.

| Function / macro | What it does | Results beyond the common ones |
|---|---|---|
| `int ps3dk_prof_gpu_attach(ps3dk_prof_gcm_ctx *ctx, uint32_t report_first, uint32_t report_count, uint32_t label, uint32_t *gpu)` | attaches a domain (id 1026..1089) to a quiescent context with a report range (2..`gpu_report_slots_max`) and a label Does not use the common checks: `EBUSY` (inside a drain or GPU call); `ENOTINIT`; `ENOTSUP` (no GPU domains, or a rolling capture); `EINVAL` (NULL `ctx`/`gpu`; `report_count` not 2..`gpu_report_slots_max`; reports beyond 2048; `label` not 64..255; overlap with another domain or the AUTO lease); `ESTOPPED` (fault, shutdown, seal, or a failed unlock: then `*gpu` is still written and the slot is quarantined); `EFULL` (64 lifetime ids used, or no free slot); `ETHREAD`; `OK` |
| `int ps3dk_prof_gpu_begin(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_desc *desc, ps3dk_prof_gpu_zone *out)` / `PS3DK_PROF_GPU_ZONE_BEGIN(gpu, ctx, name, zone)` | begins a GPU zone (descriptor scope 2) | `EINVAL` (NULL `out`; scope not 2; NULL or refused descriptor); `ETHREAD` (unregistered thread); `ESEQLIMIT` (nesting or sequence overflow); `ESTOPPED` (domain not admitted). Outside a capture window it returns `OK` with an INERT zone and checks nothing else |
| `int ps3dk_prof_gpu_end(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_gpu_zone *zone)` / `PS3DK_PROF_GPU_ZONE_END(gpu, ctx, zone)` | ends it | `EINVAL` (NULL or INERT zone, another domain's zone, or not the innermost open zone); `ESEQLIMIT`; `ESTOPPED` |
| `int ps3dk_prof_gpu_mark(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_desc *desc)` / `PS3DK_PROF_GPU_MARK(gpu, ctx, name)` | a point marker | `EINVAL` (NULL or refused descriptor, scope not 2); `ETHREAD`; `ESEQLIMIT` (mark depth 64 or sequence overflow); `ESTOPPED` |
| `int ps3dk_prof_gpu_frame(uint32_t gpu, ps3dk_prof_token frame)` | associates the domain's work with a frame token (or NONE) | `EINVAL` (nonzero `reserved`; owner 0 with a nonzero frame; an unknown owner; a frame that thread has not ended yet); `EBUSY` (every owner entry still leased); `ESTOPPED` (shutdown, lock failure) |
| `int ps3dk_prof_gpu_calibrate(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, uint64_t timeout_us)` | records an RSX/PPU clock bracket | `EBUSY` (a calibration already active, or no free report slot or bracket credit); `ECLOCK` (clock failed or did not advance); `ETIMEDOUT` (deadline reached); `ESTOPPED` (not admitted, fault, anomaly, or cancelled by finalization) |
| `int ps3dk_prof_gpu_detach(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, uint64_t timeout_us)` | requests detach; the drainer finalizes | `ETIMEDOUT` (still pending: call again); `ECLOCK`; `ESTOPPED` (fault, sealing or sealed; also replaces `OK` if a fault latched during the wait); `OK` once retired |
| `int ps3dk_prof_gpu_release(uint32_t gpu, uint32_t *released_or_null)` | returns the reports and label for reuse; the context must be idle | `EBUSY` (not retired, a call in flight, the detach still being acknowledged, or already released); `ESTOPPED` (lock or unlock failure: `*released_or_null` may still be 1); `OK` |
| `int ps3dk_prof_gpu_get_stats(uint32_t gpu, ps3dk_prof_gpu_stats *st)` | copies counters | `EINVAL` (NULL `st`); `ESTOPPED` (lock or unlock failure, `*st` not written); `OK` |

Common results (every call except attach): `EBUSY` (inside a drain or another GPU call, or too many concurrent
users of the domain); `ENOTINIT`; `ENOTSUP` (no GPU domains); `ESTOPPED` (extension fault; shutdown for producer
calls); `EINVAL` (stale or zero handle, or a different `ctx`); `EPERM` (not the thread that attached the domain).

Off build: the three macros return `ENOTSUP`; the functions are declared in every build but exist only in the
extension archives.

```c
ps3dk_prof_gpu_zone z = PS3DK_PROF_GPU_ZONE_INIT;
PS3DK_PROF_GPU_ZONE_BEGIN(gpu, ctx, "shadow-pass", z);
draw_shadows(ctx);
PS3DK_PROF_GPU_ZONE_END(gpu, ctx, z);
```

PC app equivalent: none (app-defined zones). The RSX frames page lists RSX frame captures in a file; requesting one
from the PC app is card t_1eec0d29. There is no public app API for RSX frame capture either.

---

## 8. AUTO RSX observation: tags, ranges, options (`prof_auto.h`)

Archive: `libps3dkprof_auto.a` via `PS3_PROF_AUTO=ON`. The tag/range/seal macros are inert (with the values shown)
outside AUTO units. Call them on the owner thread (the thread that owns the attached GCM context); elsewhere they are
inert.

| Entry | Signature / value | Notes | PC app equivalent |
|---|---|---|---|
| `PS3DK_PROF_AUTO_TAG(t)` | `void ps3dk_prof_auto_tag(uint32_t tag)` | later operations record tag `t` (a segment draw: the tag at build; a whole-CALL record: the tag at the CALL). Off: nothing | none (Passes page reads tags) |
| `PS3DK_PROF_AUTO_RANGE_BEGIN(name)` | `int ps3dk_prof_auto_range_begin_at(const char *name, const char *file, uint32_t line, uint32_t timed)` with `timed` 0 | RSX time between BEGIN and END by its own report pair. 1 admitted, 0 refused (counted). Nesting depth 4; refused inside a segment build, across frames, or without a reserved pair. Off: 1 | none (Passes page reads ranges) |
| `PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name)` | same with `timed` 1 | its covered draws also get per-draw pairs | none yet (per-capture timing, card t_6dd7fb83) |
| `PS3DK_PROF_AUTO_RANGE_END()` | `int ps3dk_prof_auto_range_end(void)` | 1 committed, 0 refused; issue END whatever BEGIN returned. Off: 1 | none |
| `PS3DK_PROF_AUTO_TIMED_RANGES_ONLY()` | `void ps3dk_prof_auto_timing(uint32_t mode)` with mode 1 | per-draw timing only inside timed ranges; other draws are covered, untimed ("outside timed range"). Off: nothing | none yet (per-capture timing, card t_6dd7fb83) |
| `int ps3dk_prof_auto_range_begin(ps3dk_prof_desc *desc, uint32_t timed)` | caller-owned descriptor form | | none |
| `PS3DK_PROF_AUTO_SEAL()` | `ps3dk_prof_capture_seal()` | before an exit that skips `atexit` (`_exit`, a quit-guard deadline), on the owner thread (the init thread). `EPERM` elsewhere, `EBUSY` in a callback, `EALREADY` after shutdown. Never waits on the RSX. Off: 0 | none yet (finish capture, card t_dd5b7ce1) |
| `PS3DK_PROF_AUTO_SEGMENT_DRAW_WORDS` | 4u in AUTO units, 0u otherwise | extra words per covered draw inside a CALL segment | n/a |

Range names are string literals; the runtime keeps one descriptor per name/file/line site (256 sites). Before the
owner's first frame there is no frame, so ranges are refused. Details: [profiler-auto-tags.md](profiler-auto-tags.md).

### `ps3dk_prof_auto_options` and `int ps3dk_prof_auto_options_set(const ps3dk_prof_auto_options *options)`

```c
typedef struct ps3dk_prof_auto_options {
    uint32_t size;              /* 52; the older 36 and 32 are accepted */
    uint32_t report_first;      /* local reports, first index 0..2047 */
    uint32_t report_count;      /* even, 18..1024, within 0..2047 */
    uint32_t operation_label;   /* 64 or 69..254, distinct from clock_label */
    uint32_t clock_label;
    uint32_t metadata_records;  /* 1..1024 */
    uint32_t frame_records;     /* 1..256 */
    uint32_t max_bytes;         /* <= 2 MiB */
    uint32_t range_pairs;       /* 0: default (an eighth of the pool, at most 64; 63 with 1024 reports) */
    uint32_t ring_blocks;       /* 0: no ring; 2 or 3 selects the report ring */
    uint32_t ring_label;        /* 0: 245; else 64 or 69..254, distinct */
    uint32_t ring_bytes;        /* 0: 16 MiB; whole MiB up to 256 MiB */
    uint32_t ring_records;      /* 0: 16384; 1..65536 */
} ps3dk_prof_auto_options;
#define PS3DK_PROF_AUTO_OPTIONS_INIT {52u,1024u,1024u,247u,246u,1024u,256u,2097152u,0u,0u,0u,0u,0u}
```

`ps3dk_prof_auto_options_set` (macro `PS3DK_PROF_AUTO_OPTIONS_SET(o)`, `0` outside AUTO) validates and copies options
for the next intercepted `cellGcmInit` on the same thread. Another thread's init takes the defaults and marks the
capture `PREINIT_FOREIGN`. Returns `OK`; `EINVAL` (options attach would refuse); `EBUSY` (an intercepted `cellGcmInit`
already ran); `ENOTINIT` (before profiler start-up); `ETHREAD`. A later call replaces an earlier one.
PC app equivalent: none yet (per-capture timing, card t_6dd7fb83; startup knobs, card t_9d1f2c10).

```c
ps3dk_prof_auto_options o = PS3DK_PROF_AUTO_OPTIONS_INIT;
o.range_pairs = 32;
PS3DK_PROF_AUTO_OPTIONS_SET(&o);
cellGcmInit(...);
```

### Build-time ring options

```c
typedef struct ps3dk_prof_auto_ring_options {
    uint32_t size;          /* 16 */
    uint32_t ring_blocks;   /* 2 or 3 (the generated constant uses 2) */
    uint32_t ring_bytes;    /* 1..256 MiB in whole MiB */
    uint32_t ring_records;  /* 1..65536 */
} ps3dk_prof_auto_ring_options;
#define PS3DK_PROF_AUTO_RING_OPTIONS_INIT(bytes, records)
#define PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS(...)  /* defines const ps3dk_prof_automatic_ring_options */
```

Normally generated by `PS3_PROF_AUTO_RING=ON`. Read before init through a weak reference; an invalid definition
prints a diagnostic and the automatic capture is not initialized. `PS3DK_PROF_AUTO_OPTIONS_SET` wins over it.
PC app equivalent: none yet (startup knobs, card t_9d1f2c10).

### `int ps3dk_prof_auto_attach(ps3dk_prof_gcm_ctx *context, const ps3dk_prof_auto_options *options, uint32_t *instance, uint32_t *domain)`

Optional override on the registered context owner before its first covered operation, clock or frame observation.
NULL options select the defaults. Refused after any command write (`PRIOR_WRITES_UNKNOWN`); both outputs are 0 on
refusal. No second instance. The intercepted `cellGcmInit` attaches automatically; most apps never call this.
PC app equivalent: none.

### `int ps3dk_prof_auto_frame_end(uint32_t instance, ps3dk_prof_token frame)`

Closes this owner's active matching coverage frame without waiting for the RSX. Missing, foreign or duplicate
closure is refused; reserved token fields must be 0.
PC app equivalent: none.

---

## 9. Waits (`prof_wait.h`)

Archive: `libps3dkprof_wait.a` / `libps3prof_wait.a` / `libps3dkprof_ext_wait.a`, selected by `PS3_PROF_WAITS`.
The observations need no app call; this optional setter changes the limits.

```c
typedef struct ps3dk_prof_wait_config {
    uint32_t size;          /* sizeof(ps3dk_prof_wait_config) */
    uint32_t max_bytes;     /* 1..8 MiB, default 1 MiB */
    uint32_t max_actors;    /* 1..1024 and <= max_threads; default min(max_threads, 64); counts lifetime registrations */
    uint32_t ring_records;  /* power of two 8..4096, default 128 */
} ps3dk_prof_wait_config;
int ps3dk_prof_wait_configure(const ps3dk_prof_wait_config *cfg);
```

Before init. Returns `OK`; `EINVAL` (bad size or range, or the layout exceeds `max_bytes`); `EBUSY` (another setter or
init running); `EALREADY` (after init). A `max_actors` above `max_threads` fails init with `EINVAL`.
PC app equivalent: none yet (startup knobs, card t_9d1f2c10). The Waits page shows the observations.

---

## 10. FIFO wrap observation (`prof_fifo.h`)

Archive: `libps3dkprof_ext_fifo.a` + `libgcm_cmd_prof_fifo.a` (selector `ps3_target_prof_fifo`). The capture must
run THREAD mode.

```c
typedef struct ps3dk_prof_fifo_options {
    uint32_t size;          /* 16 */
    uint32_t ring_records;  /* 0 = 1024; else 16..65536 events of 96 bytes */
    uint32_t max_bytes;     /* 0 = 1 MiB; else 5632..16 MiB */
    uint32_t reserved;      /* 0 */
} ps3dk_prof_fifo_options;
#define PS3DK_PROF_FIFO_OPTIONS_INIT {16u, 0u, 0u, 0u}

int ps3dk_prof_fifo_attach(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, const ps3dk_prof_fifo_options *options,
                           uint32_t *instance);
int ps3dk_prof_fifo_detach(uint32_t instance);
```

`attach` on the GCM context owner (the thread that attached GPU domain `gpu` for `ctx`), registered, context
quiescent; NULL options take the defaults. Returns `OK` with a nonzero instance; on refusal `*instance` is 0:
`EBUSY` (inside a drain or GPU call); `ENOTINIT`; `ENOTSUP` (no GPU domains); `EPERM` (APP mode, no drain thread
yet, or not the thread that owns the domain); `EINVAL` (NULL `ctx`/`instance`; bad `size`/`reserved`; ring or ceiling
out of range; a stale domain or another `ctx`; the GCM bridge refused the ring); `ESTOPPED` (shutdown, a sealed
instance, or the seal won); `ETHREAD` (unregistered thread, or owner identity failed); `EFULL` (a second attach in the
capture); `ENOMEM`.
`detach` on the owner: removes the observer, never waits, and makes no init check. `EINVAL` (0 or another instance,
or not attached); `EPERM` (another thread); `EBUSY` (inside the observer callback); `EALREADY` (already detached or
closed); `ESTOPPED` (sealed: the observer is removed and its storage stays allocated); `OK`.
AUTO builds contain FIFO observation without these calls.
PC app equivalent: none. The FIFO wrap waits page shows the events.

---

## 11. Counters and function paths (`prof_counter.h`, `prof_func.h`)

### `int ps3dk_prof_counter(const char *name, int64_t value)` / `PS3DK_PROF_COUNTER(name, value)`

Archive: `libps3dkprof_counters.a` or `libps3dkprof_ext_counters.a`. Records one sample (thread, counter, frame
marker, timebase, value). `name`: 1..64 bytes of printable ASCII without spaces; at most 64 names
(`PS3DK_PROF_COUNTER_NAMES_MAX`, `PS3DK_PROF_COUNTER_NAME_MAX`); 4096 samples held between drains
(`PS3DK_PROF_COUNTER_BUFFER`). Returns `OK`; `ENOTINIT`; `ESTOPPED`; `EBUSY` (inside a drain or callback); `EPERM`
(thread without an incarnation); `EINVAL` (bad name); `EFULL` (name table full, or buffer full until the next drain).
Refused and dropped calls are counted. Takes one mutex; never blocks on the sink. Off build: `OK`.
PC app equivalent: none. Overview COUNTERS and `ps3dk-prof counters` show the values.

```c
PS3DK_PROF_COUNTER("draws", draw_count);
```

### `int ps3dk_prof_func_configure(const ps3dk_prof_func_config *cfg)`

Archive: `libps3dkprof_func.a` or `libps3dkprof_ext_func.a`, with sources compiled `-finstrument-functions`.

```c
typedef struct ps3dk_prof_func_config {
    uint32_t size;          /* sizeof(ps3dk_prof_func_config) */
    uint32_t max_bytes;     /* bound on all function-path storage; default 8 MiB */
    uint32_t max_actors;    /* 1..min(1024, max_threads); default at init min(max_threads, 8) */
    uint32_t node_capacity; /* 4..16384 per actor, including the LOST node; default 4096 */
    uint32_t stack_depth;   /* 4..256; default 256 */
    uint32_t reserved;      /* 0 */
} ps3dk_prof_func_config;
```

Without a call, every field takes its default at init. A 0 field also takes its default, except `max_actors`:
`ps3dk_prof_func_configure` checks the layout before the thread count is known, so it refuses `max_actors` 0 with
`EINVAL`; set it explicitly (1..1024). Before init. Returns `OK`; `EINVAL` (NULL, wrong `size`, nonzero `reserved`,
a field out of range, or a layout larger than `max_bytes`); `EBUSY` (another setter or init running); `EALREADY`
(after init). Init fails with `EINVAL` if `max_actors` exceeds `max_threads` or the layout does not fit, and
`ENOMEM` if the storage cannot be allocated.
PC app equivalent: none yet (functions switch, card t_2873fe00; startup knobs, card t_9d1f2c10). Hot paths ->
Functions (automatic) shows the trees.

---

## 12. Live link (`prof.h`)

Archive: `libps3dkprof.a` (manual variants of counters, func and waits too), or `libps3dkprof_ext_live.a` in
extension builds. Link `net`; call `netInitialize()` first. Manual lifecycle only: no build starts it.

```c
typedef struct ps3dk_prof_live_config {
    uint16_t port;                 /* default 31750 */
    uint16_t reserved;             /* 0 */
    uint32_t poll_us;              /* 1000..1000000; default 10000 */
    uint32_t send_timeout_us;      /* 1000..60000000; default 2000000 */
    uint32_t handshake_timeout_us; /* 1000..60000000; default 2000000 */
    uint32_t stack_bytes;          /* 16 KiB..1 MiB; default 64 KiB */
    uint32_t reserved2;            /* 0 */
    uint64_t cap_bytes;            /* 1..2^40; default 1 GiB served to one viewer */
} ps3dk_prof_live_config;

typedef struct ps3dk_prof_live_stats {
    uint32_t version;              /* 1 */
    uint32_t reserved;
    uint64_t connections, busy_refusals, bytes_delivered, pongs;
    uint64_t drop_end, drop_handshake_timeout, drop_bad_hello, drop_protocol, drop_ping_flood,
             drop_stalled, drop_peer_closed, drop_io, drop_cancelled, drop_clock;
} ps3dk_prof_live_stats;
```

Zero fields take defaults; NULL takes every default; out-of-range values or nonzero reserved fields are `EINVAL`.
Each finished admitted connection ends in exactly one `drop_*` reason.

### `int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg)` / `PS3DK_PROF_LIVE_START(cfg)`

After a successful init. Starts one agent thread that serves this run's file to one viewer at a time and applies
viewer capture triggers (protocol 2) inside `ps3dk_prof_frame`. Returns `OK`; `ENOTINIT`; `ESTOPPED`; `ENOTSUP`
(callback sink, rolling capture, capture outside `/dev_hdd0/`); `EALREADY` (once per run); `EINVAL`; `ENET`;
`ETHREAD`. Caller-serialized. Off build: `ENOTSUP`.
PC app equivalent: **Connect…** connects to a running agent; starting the agent without app code is card
t_276ffda7.

### `int ps3dk_prof_live_stop(uint64_t graceful_us, uint64_t join_us)` / `PS3DK_PROF_LIVE_STOP(g, j)`

Call after `ps3dk_prof_shutdown` (a streaming viewer needs its END). Lets the viewer finish for up to `graceful_us`
(0 cancels at once), then waits up to `graceful_us + join_us` for the thread. Returns `OK`; `ETIMEDOUT` / `ECLOCK`
(still stopping: call again, keep the network up); `ETHREAD` (terminal: never tear down the network); `EALREADY`;
`ENOTINIT`; `EINVAL` (deadline overflow). The deadlines do not bound an LV2 file call in progress. Off build:
`ENOTSUP`.
PC app equivalent: **Disconnect** ends the PC session only; the agent keeps running.

### `int ps3dk_prof_live_get_stats(ps3dk_prof_live_stats *out)` / `PS3DK_PROF_LIVE_GET_STATS(out)` (EXPERIMENTAL)

After a successful stop; readable again later and after shutdown. Returns `OK`; `EINVAL` (NULL); `ENOTINIT` (no
start succeeded); `EBUSY` (agent running or stopping); `ETHREAD` (failed join). Off build: `ENOTSUP`.
PC app equivalent: none; the viewer reports its own session outcome.

```c
if (netInitialize() == 0 && PS3DK_PROF_LIVE_START(NULL) != PS3DK_PROF_OK)
    netDeinitialize();
```

---

## 13. Rolling output (`prof_rolling.h`)

Archive: `libps3dkprof_auto.a` only (other archives return `ENOTSUP` for an enabled request). Normally configured by
`PS3_PROF_AUTO_ROLLING=<MiB>`.

```c
#define PS3DK_PROF_ROLLING_APPEND 0u            /* mode */
#define PS3DK_PROF_ROLLING_ENABLED 1u
#define PS3DK_PROF_ROLLING_PROFILE_NONE 0u      /* profile; only AUTO_FIFO is public */
#define PS3DK_PROF_ROLLING_PROFILE_PPU 1u
#define PS3DK_PROF_ROLLING_PROFILE_SPU 2u
#define PS3DK_PROF_ROLLING_PROFILE_AUTO_FIFO 3u
#define PS3DK_PROF_ROLLING_NONE 0u              /* result state */
#define PS3DK_PROF_ROLLING_OPEN 1u
#define PS3DK_PROF_ROLLING_SEALED 2u
#define PS3DK_PROF_ROLLING_STOPPED 3u
#define PS3DK_PROF_ROLLING_REASON_NONE 0u       /* stop reason */
#define PS3DK_PROF_ROLLING_CHECKPOINT_BYTES 1u
#define PS3DK_PROF_ROLLING_STATE_BYTES 2u
#define PS3DK_PROF_ROLLING_IDENTITIES 3u
#define PS3DK_PROF_ROLLING_SUPPORT 4u
#define PS3DK_PROF_ROLLING_COUNTER_RANGE 5u
```

`ps3dk_prof_rolling_options` (192 bytes, both ABIs): `size, version, mode, profile`; `max_capture_bytes,
max_fixed_state_bytes, prepare_work_limit`; `retained_segments, checkpoint_bytes, segment_payload_bytes,
metadata_bytes`; `incarnations, descriptors, ppu_stack_rows, frame_identities`; `spu_domains, spu_lanes, spu_images,
epochs, brackets, image_descriptors`; `journal_entries, event_runs, live_spu_stack_rows, saved_spu_witnesses`;
`rsx_domains, windows, fifo_membership_runs, accounting_groups`; `auto_final_runs, auto_touched_runs,
auto_build_entries, auto_clock_anchors`; `auto_bootstrap_bytes, auto_support_bytes`; `reserved[10]` (0). Field
meanings and limits: [ps3dk-prof-rolling-runtime.md](design/ps3dk-prof-rolling-runtime.md).

`PS3DK_PROF_ROLLING_OPTIONS_INIT`: append mode (size 192, version 1). `PS3DK_PROF_ROLLING_AUTO_FIFO_INIT(file_limit,
fixed_limit)`: the finite AUTO/FIFO preset (six retained segments plus a spare; raising only `file_limit` does not
enlarge the file).

`ps3dk_prof_rolling_result` (128 bytes): `size, version, format, profile, original_flags, state, reason, reserved0`;
`accepted_next_chunk, accepted_payload_bytes, committed_next_chunk, committed_payload_bytes`; `revision, file_bytes,
fixed_state_bytes`; `result, io_result, close_result, join_result`; `reserved[6]`.

| Entry | Notes | PC app equivalent |
|---|---|---|
| `int ps3dk_prof_set_rolling_options(const ps3dk_prof_rolling_options *)` / `PS3DK_PROF_SET_ROLLING_OPTIONS(p)` | before init; copies on success, keeps the prior options on refusal (`EINVAL`, `ENOTSUP`, gate results). AUTO initializes before `main`, so use the constant below there. Off: `ENOTSUP` | none yet (viewer rolling, cards t_149a6545 / t_914a084f) |
| `PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(...)` | defines `const ps3dk_prof_rolling_options ps3dk_prof_automatic_rolling_options`, read by the AUTO constructor; put it in an executable source, not a static archive; an invalid one prints "rolling configuration refused" and skips init | none yet (t_149a6545 / t_914a084f) |
| `int ps3dk_prof_get_rolling_result(ps3dk_prof_rolling_result *)` / `PS3DK_PROF_GET_ROLLING_RESULT(p)` | after shutdown has joined; set `size` 128 and `version` 1; SEALED alone does not prove success (check `result`, `close_result`, `join_result`). Off: `ENOTSUP` | the Rolling page shows the state of an opened `.profr` |

```c
/* custom budget, in an executable source of an AUTO build */
#include <ps3dk/prof_rolling.h>
PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(
    PS3DK_PROF_ROLLING_AUTO_FIFO_INIT(UINT64_C(128) << 20, UINT64_C(64) << 20));
```

---

## 14. Snapshot and export

There is no app-side API for snapshots or exporting part of a capture.

| Action | Where |
|---|---|
| Freeze one frame | PC app **Capture full frame** (Snapshot page) |
| Write one window or a frame range as its own `.prof` | PC app **Save window…** / **Save range…** on an opened file; `ps3dk-prof export --window W \| --frame N \| --frames A..B in.prof out.prof` |
| List windows | Overview WINDOWS; `ps3dk-prof windows` |

---

## 15. Result codes (`prof.h`)

| Code | Value | Meaning |
|---|---|---|
| `PS3DK_PROF_OK` | 0 | success |
| `PS3DK_PROF_EPERM` | -1 | APP mode: not the drain (init) thread; window END from another thread; live trigger END on the app's window |
| `PS3DK_PROF_EBUSY` | -2 | inside a drain, a drain running, init or a setter running; a window open or opening |
| `PS3DK_PROF_EALREADY` | -3 | init already succeeded, shutdown already called, thread already begun, live already started |
| `PS3DK_PROF_EEXIST` | -4 | the capture file exists (exclusive create) |
| `PS3DK_PROF_EINVAL` | -5 | a configuration or argument out of range |
| `PS3DK_PROF_ENOMEM` | -6 | allocation failed |
| `PS3DK_PROF_ENOTQUIESCED` | -7 | trailer end_flags bit 0: threads did not quiesce |
| `PS3DK_PROF_ESEQLIMIT` | -8 | end_flags bit 1: a sequence or counter ceiling was reached |
| `PS3DK_PROF_ESINKRECOVERED` | -9 | end_flags bit 2: sink errors were recovered |
| `PS3DK_PROF_ESINKFAIL` | -10 | the sink failed; no trailer |
| `PS3DK_PROF_ECLOSE` | -11 | close failed after the trailer: the run fails |
| `PS3DK_PROF_ETHREAD` | -12 | thread identity, creation or join failed |
| `PS3DK_PROF_ECLOCK` | -13 | timebase frequency or monotonic clock unavailable |
| `PS3DK_PROF_ENOTINIT` | -14 | not initialized |
| `PS3DK_PROF_ESTOPPED` | -15 | the stop latch is set |
| `PS3DK_PROF_EFULL` | -16 | no free thread slot, incarnation ids exhausted, a queue or table full |
| `PS3DK_PROF_ENOTSUP` | -17 | not supported here: callback sink, unmeasured device, rolling with live, off build |
| `PS3DK_PROF_ENET` | -18 | live: the listener could not be created |
| `PS3DK_PROF_ETIMEDOUT` | -19 | live stop / lane end / GPU detach not finished: call again; trigger not applied within 5 s |
| `PS3DK_PROF_ERETENTION` | -32 | capture closed, completed-file bookkeeping or cleanup failed |
| `PS3DK_PROF_ERESOURCE` | -33 | rolling stopped at a whole-chunk boundary: a configured bound was reached |

-20..-23 belong to the SPU lane writer (section 6).

---

## 16. Parallels: app API and PC app

| App API action | PC app action | Status |
|---|---|---|
| `ps3dk_prof_init` (manual lifecycle) | none | none yet: live auto-start t_276ffda7, startup knobs t_9d1f2c10 |
| config fields, `set_sink_path`, `set_capture_retention` | none | none yet: startup knobs t_9d1f2c10 |
| `ps3dk_prof_live_start` | **Connect…** (needs the agent running) | partial: connect works; starting the agent from the PC is t_276ffda7 |
| `ps3dk_prof_live_stop` | **Disconnect** (ends the PC session only) | partial |
| `PS3DK_PROF_CAPTURE_BEGIN` | **Begin** | available (live) |
| `PS3DK_PROF_CAPTURE_END` | **End** (windows the PC opened) | available (live) |
| `PS3DK_PROF_CAPTURE_FRAMES` | **Capture N frames** | available (live) |
| `PS3DK_PROF_CAPTURE_WHOLE_RUN` | none | none yet: default capture window t_7adaacd4 |
| `PS3DK_PROF_SECTION` | Begin/End by hand | none yet: until-marker windows t_3ea33a77 |
| `PS3DK_PROF_CAPTURE_STATE` | trigger answers with window id; Overview WINDOWS | available |
| `ps3dk_prof_shutdown`, `capture_seal`, `PS3DK_PROF_AUTO_SEAL` | none | none yet: finish capture t_dd5b7ce1 |
| `PS3DK_PROF_ZONE*` on or off | none | none yet: zones switch t_2873fe00 |
| SPU lanes (`init_ex`, `spu_lane_create`) | none | none yet: SPU switch t_2873fe00 |
| Function paths (link + `func_configure`) | none | none yet: functions switch t_2873fe00 |
| `PS3DK_PROF_AUTO_TIMED_RANGES_ONLY`, `RANGE_BEGIN_TIMED`, `AUTO_OPTIONS_SET` | none | none yet: per-capture timing t_6dd7fb83 |
| AUTO ring options, `wait_configure` | none | none yet: startup knobs t_9d1f2c10 |
| `set_rolling_options`, automatic rolling options | none | none yet: viewer rolling t_149a6545 / t_914a084f |
| `get_rolling_result` | Rolling page of an opened `.profr` | available (offline) |
| RSX frame capture | RSX frames page lists captures in a file | none yet: request from the PC t_1eec0d29 |
| `PS3DK_PROF_FRAME`, `PS3DK_PROF_AUTO_TAG`, AUTO ranges, GPU zones, counters, FIFO attach | none (app-defined data) | app only |
| none | **Capture full frame** | PC only |
| none | **Save window…** / **Save range…** | PC only (file opened from disk); also `ps3dk-prof export` |

# libps3dkprof step 3, host half: code review map

**Status:** written, never compiled or run (codex 23:55: code only). Host build and run is `tests/host/ps3dk-prof-runtime/build-and-run.sh <out-dir>`. Target compilation, `pal_lv2.c`, SDK delivery (`SUBLIBS`), packaging and landing are NOT part of this step.

**Contract:**
- the normative format `docs/design/ps3dk-prof-format.md` at `bece3bd3`, plus the one edit quoted in section 1;
- `STAGE3-PLAN-r2.md` (`a11a5861`), plus ERRATUM-1 (`5a075d04`) and ERRATUM-2 (`6b0d9e74`), where the later document supersedes the earlier one;
- the codex 23:55 boundary correction.

Line numbers are for the files as delivered. `core` means `src/core/prof_core.c` and `drain` means `src/core/prof_drain.c`.

## 1. Counter exhaustion: the fail-closed design (boundary correction)

**Rule.** The limit for every exact counter and every id counter is 2^63. No counter is wrapped, and none is saturated while the capture still claims an exact count.

- An increment that would take a counter past 2^63 is **not applied**.
- In its place, `exhaust()` (core:203) sets `cb->limit` (SEQ_LIMIT, limit_flags bit0) and the diagnostic `cb->exhausted`.
- From then on the incarnation records nothing more, in the same way as at the sequence limit (`seq_admit`, core:237, refuses everything once `limit` is set).

**Per counter:**

| Counter | Where | At the ceiling |
|---|---|---|
| `refused_count`, +1 or +k | `count_refused` core:210 | The increment is skipped and the incarnation exhausts. Every refusal and every misuse goes through this function. |
| `refused_count`, STOP batch not admitted | `p_observe_stop` core:507-511 | `+= k` only if `refused <= 2^63 - k`. Otherwise the count is unchanged and `exhausted` is set. `limit` is set either way. |
| `regerr_count` | `regerr` core:326 | No increment, no REGERR event, exhaust. A REGERR event is attempted only after its increment has been applied, so a REGERR record never outnumbers its counter (format 6). |
| span ids | `zone_begin` core:602 | `next_span_id >= 2^63` exhausts and refuses the begin, which is counted through `count_refused`. Ids handed out are at most 2^63 - 1. |
| refused-begin ids | `zone_begin` core:626 | The begin is counted, the incarnation exhausts, and the handle is INERT. This is F1's "counts nothing" corrected by the boundary rule: the refusal is still counted when that is exact. |
| frame ids | `ps3dk_prof_frame` core:708 | Exhaust and refuse. Unreachable, because a frame id is at most its own seq. |
| sequences | `seq_admit` core:237 | Unchanged format 5.1 rule: refused whole, SEQ_LIMIT. |
| incarnation ids | `thread_begin_impl` core:451 | `next_incarnation = 0` after UINT32_MAX. Later registrations get EFULL and `unregistered_refusals` goes up (5.4). |

**Why this is never a clean claim.**
- A refusal or registration error that could not be counted only happens after `limit` is set: it is set in the same call.
- Every final entry with `limit` carries limit_flags bit0:
  - RETIRED, drain:405;
  - ACKED, drain:581.
- So end_flags bit1 is set (drain:569), the reader says INCOMPLETE, and shutdown returns ESEQLIMIT. ESEQLIMIT is outranked only by ESINKFAIL, ECLOSE and ENOTQUIESCED, which are also failures.
- Counters written to the file are always at most 2^63 and exact for every event counted before the limit. Nothing is ever written above 2^63.

**Normative edit.** This is the minimal edit to format section 5.1; the rest of the document is unchanged:

> - **Counter ceiling (fail-closed).** `refused_count` and `regerr_count` are never wrapped, saturated or allowed past 2^63, and neither are the producer's id counters (span ids, refused-begin ids, frame ids).
>   - An increment that would take `refused_count` or `regerr_count` past 2^63 is NOT applied. The incarnation instead sets SEQ_LIMIT (limit_flags bit0), and from then on records nothing more, as for the sequence limit.
>   - So does an id counter that would pass 2^63.
>   - The counters therefore stay exact for every event counted before the limit. A refusal or registration error that could not be counted only ever happens after SEQ_LIMIT is set, so the capture ends INCOMPLETE (end_flags bit1) and never claims a clean measurement.
>   - A STOP batch that is not admitted adds k to `refused_count` only if the sum stays at most 2^63. Otherwise the count is left unchanged, and SEQ_LIMIT (which a non-admitted batch sets anyway) records that the batch's refusals were not counted.
>   - A REGERR event is attempted only after its `regerr_count` increment was applied.

**Tests:**

| Test | What it checks |
|---|---|
| `rt_counter_refused_misuse_at_ceiling` | A misuse at `refused = 2^63`. |
| `rt_counter_refused_at_ceiling_id_below` | `refused` reaches 2^63 exactly while the refused id is still below its ceiling, so the handle works LIFO. |
| `rt_counter_regerr_at_ceiling` | `regerr` reaches 2^63 exactly with its REGERR written; a further failure then has no REGERR. |
| `rt_counter_refused_id_exhausted` | The refused-begin id counter at its ceiling. |
| `rt_counter_span_id_exhausted` (+) | The span-id counter at its ceiling. |
| `rt_counter_stop_batch_refused_at_ceiling` | A STOP batch that is not admitted, with `refused` + k past 2^63. |

All six are in `rt_t_counters.c`. Each asserts:
- the exact counter values;
- `limit` and `exhausted`;
- ESEQLIMIT;
- end_flags bit1;
- reader exit 1.

## 2. Files

| File | Role |
|---|---|
| `sdk/include/ps3dk/prof.h` | The API, the types, the enabled and disabled macros, the C++ guard, the SPU `#error`. In `sdk/Makefile` HEADERS, not in SUBLIBS. |
| `src/pal/pal.h`, `src/pal/pal_host.c` | The PAL interface and the POSIX implementation. Test faults are injected here. |
| `src/core/prof_internal.h` | Structures, atomics, constants, the `ps3dk_prof__` symbol renames, the hook macros. |
| `src/core/prof_test_hooks.h` | The `PS3DK_PROF_TEST_HOOKS`-only hook table, presets and statistics. |
| `src/core/prof_core.c` | Init and the setters, threads, zones, frames, the STOP batch, descriptor registration. |
| `src/core/prof_drain.c` | The encoder, the sink, ordinary drains, the drain thread, quiescence, the frontier, sealing, drain and shutdown. |
| `tests/host/ps3dk-prof-runtime/` | `rt.h`, `rt_util.c` (setup, events, the memory sink, the mini reader), `rt_main.c` + `rt_list.h` (the registry), `rt_t_*.c` and `rt_cxx.cpp` (the tests), the off-build sources (`ob_same_source.c`, `ob_pp_check.py`, `probe_g.cpp`), and `build-and-run.sh`. |

## 3. Rule-to-code map

### Format 5.1: sequences, loss, refusal

| Rule | Code |
|---|---|
| Sequence assignment, `produced = next_seq` | `seq_assign` core:248 |
| Admission and the 2^63 boundary; a 2-slot BEGIN is refused whole | `seq_admit` core:237; used by `zone_begin` core:606, `emit1` core:303 and `frame` core:704 |
| Whole-publication space check before any write; drop into the pending range | `reserve` core:259 |
| GAP + event as one publication with ONE release; `pub_len` marking (r2 7) | `reserve` writes the GAP; `commit` core:285 |
| Dropping never changes the stack (a dropped BEGIN is pushed and uses its id) | `zone_begin` core:608-619 |
| Refused events assign no sequence and are counted | `count_refused` callers: core:307, 625, 671, 679, 705 |
| STOP batch admitted whole: published, dropped whole, or refused whole; k = 0 does nothing | `p_observe_stop` core:494-531 |

### Format 5.2: depth

| Rule | Code |
|---|---|
| Depth cap 256 | core:600 |
| The END of a refused BEGIN is consumed by the refused-handle LIFO | `zone_end` core:667-672 |

### Format 5.3: descriptor registration

| Rule | Code |
|---|---|
| Check, lock, re-check (Q5) | `desc_index` core:410; `desc_register` core:339 |
| The complete entry, then RELEASE of `desc_count`, then RELEASE of the call-site state | core:387-401, with the hook at core:397 |
| TABLE_FULL / ID_COLLISION: regerr unconditional (ceiling-checked), then a best-effort REGERR, outside the mutex | core:376-384, 405; `regerr` core:322 |
| The failure is cached at the call site; every zone of the site is refused | `desc_index` returns 0 on a failure code; `zone_begin` core:598 |

### Format 5.4: single consumer and drain order

| Rule | Code |
|---|---|
| APP owner identity: a failed read is never the owner (Q1) | `pal_owner_is_self` pal_host.c:46; `ps3dk_prof_drain` drain:627; shutdown drain:688 |
| drain-busy CAS; EBUSY in a drain or in THREAD mode | drain:623-630 |
| Step 1, ring snapshot: table copied under the mutex, ack word ACQUIRE before write-index ACQUIRE | `p_drain_once` drain:355-385 |
| Step 2, tables: THREADS copied under the mutex and encoded after it; DESCRIPTORS up to ACQUIRE of `desc_count` | `declare_threads` drain:247; `declare_descs` drain:270 |
| Step 3, records: whole publications only, `pub_len` validated, chunks of at most 32767 records | `drain_ring` drain:296 |
| Step 4, retirement: RETIRED entry, then the hook, then FREE under the mutex | drain:396-425 |
| Registration refused after the latch; incarnation ids start at 1; exhaustion counted | `thread_begin_impl` core:423 |

### Format 5.5 and 5.6: timebase, frames, tokens

| Rule | Code |
|---|---|
| Timebase read between compiler barriers | `p_tb` core:89 |
| FRAME takes its id with its sequence, then RELEASE of `current_frame` after the publish or the drop; a refused FRAME takes no id | `ps3dk_prof_frame` core:684 |
| Token from an ACQUIRE of `current_frame` | `ps3dk_prof_frame_token` core:723 |

### Format 7: lifecycle

| Rule | Code |
|---|---|
| One capture per process; init EALREADY forever | `gate_enter` core:747; `ps3dk_prof_init` core:995 |
| Latch RELEASE under `thread_mutex`, fixing Ft; ACQUIRE at every entry point | shutdown drain:694-711; ACQUIRE loads at core:435, 546, 584, 657, 697, 733 |
| Shutdown entry checks in order: in-drain EBUSY, APP non-owner EPERM, once EALREADY | drain:686-691 |
| STOP batch, then the ack as the LAST write (steps 1-5) | `p_observe_stop` core:503-531 |
| Retire: snapshot, clear without synthesising ENDs, then the RETIRED word last; LIVE to RETIRING allowed after the latch (E1) | `ps3dk_prof_thread_end` core:534-563 |
| `thread_end` that observes STOP acks; `thread_end` after the ack is refused | core:542-549 |
| Self-quiescence: the caller's own batch | drain:715; drain thread drain:669 |
| Quiescence wait: bounded, monotonic deadline; a clock failure ends it at once (Q8) | `quiesce_wait` drain:446 |
| Frozen frontier, once, in order 1 to 4; the classification is permanent and comes from the publication (E1) | drain:490-518 |
| No ordinary processing after the frontier (F2) | `p_drain_once` drain:347; nothing frees a slot after `frozen` |
| Final drain to Ft / Fd / F[i]; frozen-RETIRED entries in a RETIRED chunk (F2); the TRAILER | drain:522-594 |
| State-3 entries: UNKNOWN fields, `limit_flags` 0x02, exact `written` | drain:578-582 |
| Storage lifetime: one block, never freed after READY | `init_impl` core:855; `pal_free` only in the init unwind |

### Format 8: sink and result

| Rule | Code |
|---|---|
| Exclusive create; EEXIST; other open errors are ESINKFAIL and retryable (E3) | core:958-967 |
| Short writes retried; 16 tries without progress; EINTR retried and counted as recovered; a count above n, or a negative return, is a SINK FAILURE | `sink_write` drain:22 |
| Nothing written after a failure; trailer only without a failure; flush once after the trailer; close once | drain:586-594; `sink_close` drain:64 |
| Severity order: ESINKFAIL, ECLOSE, ENOTQUIESCED, ESEQLIMIT, ESINKRECOVERED, then ECLOCK, then OK; ETHREAD for a join failure only when otherwise OK | drain:596-608, 737-738 |

### Plan r2 and its errata

| Item | Code |
|---|---|
| r2 3.1 slot states, one consumer, reset in FREE under the mutex | core:455-463 (reset); drain:378-383 and 414-425 (transitions) |
| r2 3.2 + E2 + F1 handle protocol | `zone_end` core:639: step 1 core:648; 2 core:652; 3 core:655; 4 core:657; 5 core:661; REFUSED core:667; PUSHED core:674 |
| F1 identities | Refused ids core:634-635; `prev_refused_top` and `span_top_at_begin` core:595-596 |
| r2 3.3 + E3 + F3 init and gate | `init_impl` core:855: caps core:866-874; checked sizes core:892-899; step 5 core:940-951; step 6 core:953-969; step 7 core:972-981 |
| Setters | core:755, 773 |
| r2 4 atomics | `prof_internal.h` macros. GCC `__atomic` builtins only; lock-free is `_Static_assert`ed. |
| r2 7 / Q9 publication boundaries and caps | `commit` core:285; `drain_ring` drain:296; `take` core:837 |
| r2 8 / Q8 drain thread | `p_drain_main` drain:642: the start gate, one sleep slice, then the wake check |
| Q10 auto-registration | `auto_register` core:485 (opt-in; never for acked or retired TLS) |
| Hooks (r2 9 + E1) | `HOOK()` sites: core:297, 397, 530, 552, 561; drain:385, 413, 488 |

### Decisions the contract left open

| Decision | Choice and reason |
|---|---|
| `set_sink_path` | Names the capture **directory**, so Q7's snprintf-truncation EINVAL is reachable. The file is `<dir>/<app>-<16 hex of the init timebase>.prof`. Documented in `prof.h`. |
| Descriptors the format cannot carry (empty name, a scope other than 1 or 2, invalid UTF-8) | Refused (`DESC_FAILED_INVALID`) and counted as refused. Never written, because the reader would call the file MALFORMED. No REGERR, since there are only codes 1 and 2. Strings are cut to 512 bytes on a UTF-8 boundary, and the id covers the cut strings, as the reader computes it. |
| The first use of a failing site | Also refused (+1). Format 5.1 lists "every zone of a descriptor whose registration failed". |
| A PUSHED END refused at the limit | Still pops: the stack follows the calls. Its span stays OPEN in the records. |
| Zones and frames inside a drain (sink callbacks; the THREAD drain thread) | INERT, with no self-instrumentation. `thread_begin` inside a callback is allowed, which is how the drain thread becomes "registered" (`rt_drain_thread_self_acks`). |
| `unregistered_refusals` | Counts both id exhaustion and "no free slot" (EFULL). The field is informational. |
| Mutex failures | Recorded in `lock_failed`. The capture then fails closed: sealing treats it as a sink failure, ESINKFAIL. |
| Chunk buffer | The full chunk cap (16 + 1 MiB + 4), not r2 7's records-only size, because DESCRIPTORS chunks also fill to 1 MiB. Checked inside the 256 MiB cap. |
| Exported names | Exactly the API under `ps3dk_prof_`; every internal is under `ps3dk_prof__`. `build-and-run.sh` checks this with `nm`. |

## 4. Test map

Every name in the plan and its errata is implemented, apart from `rt_thread_mode_shutdown_latency_bounded`, which E5 renamed to `rt_thread_mode_wake_flag_observed_within_nominal_interval`. "(+)" marks two additions: the THREAD-mode half of `rt_shutdown_from_callback_ebusy`, and the span-id ceiling case.

| Group | Tests | Files |
|---|---|---|
| Stage-3 obligations | the 33 base rows | `rt_t_lifecycle.c`, `rt_t_stop.c`, `rt_t_race.c`, `rt_t_sink.c` |
| Q1-Q10 | 20 rows | `rt_t_lifecycle.c`, `rt_t_race.c`, `rt_t_init.c`, `rt_t_sink.c` |
| B1-B5 | 16 rows | `rt_t_race.c`, `rt_t_zones.c`, `rt_cxx.cpp`, `rt_t_init.c`, `rt_t_sink.c` |
| E1-E3 | 17 rows | `rt_t_stop.c`, `rt_t_zones.c`, `rt_t_init.c` |
| F1, F3 | 5 rows | `rt_t_zones.c`, `rt_t_init.c` |
| Boundary correction | 6 rows | `rt_t_counters.c` |
| Off-build and probes (host) | OB-E1, OB-E2 (enabled, undefined, =0; RUN), host OB-P, OB-N, PROBE-G (copy and move), archive symbols | `build-and-run.sh` |

`rt_list.h` is the authoritative list: 97 tests, each run under plain, ASan+UBSan and TSan. Every capture is checked by `ps3dk-prof check` against the verdict the test declares.

## 5. Deferred: target-only, needs the cross compiler or LV2

| Test or probe | Why it is deferred |
|---|---|
| `tgt_app_owner_sysThreadGetId`, `tgt_raw_lv2_thread_mutex_tls_identity`, `tgt_sink_eexist_lv2` | LV2 identity, librt mutex and TLS behaviour, and LV2 O_EXCL. These need `pal_lv2.c` and RPCS3 or hardware. |
| OB-P and OB-S (cross `-E` and `-S`, ILP32 and LP64), OB-SPU | Need `powerpc64-ps3-elf-gcc` and `spu-elf-gcc`. The host OB-P and OB-N run here instead. |
| PROBE-A, PROBE-L (both ABIs), PROBE-U, PROBE-O, PROBE-T | Layout, lock-free, libatomic, ordering and timebase codegen on the shipped compilers. PROBE-L's host half is the `_Static_assert` in `prof_internal.h`. |
| PROBE-H on the installed archives | No target archive exists in this step. Its host equivalent, the archive-symbol check, runs on hook-free host objects. |
| OB-H (sdk-header-list) | It runs `make install-headers` and is not run here. The HEADERS entry has been added. |

**Not covered by any named test:** the ID_COLLISION path (core:376-380). A real FNV-64 collision cannot be constructed, and the hook table has no id override. Its code shares the TABLE_FULL tail, which is tested.

## 6. Expected compile risks

- **`PS3DK_PROF_ZONE_BEGIN` statement expressions** (`__extension__({...})` with a function-local static) in C++17 with `-Wall -Wextra -Werror`. GCC and clang accept them; `-Wpedantic` is not used.
- **clang `-Wunevaluated-expression`** on the off-build `sizeof(zs[1 - hits++])`. The script adds `-Wno-unevaluated-expression` for clang only.
- **`-Wunused-but-set-variable`** on `zs` and `tok` in the disabled OB build. GCC marks sizeof operands as read. clang 13 or later probably does too, but this has not been checked.
- **`_Static_assert` and `_Thread_local`** are only in the C core and C tests. The C++ TUs include `rt.h` and `prof.h`, which use neither.
- **`posix_memalign` and `nanosleep` under `_POSIX_C_SOURCE 200809L`** in `pal_host.c`; `CLOCK_REALTIME` and `pthread_cond_timedwait` in `rt_util.c`. The core files define no feature macro and include `<pthread.h>` under strict `-std=c11`.
- **`(uint64_t)(uintptr_t)pthread_self()`** works on glibc, where `pthread_t` is an integer. On a libc where `pthread_t` is a struct it would not compile; only Linux CI is the target.
- **`snprintf` `-Wformat-truncation`** on the sink name (core:958). Its return value is checked, so level 1 should stay quiet.
- **TSan.** Tests change hook pointers while other threads run, but only on fields those threads do not read. The memory sink's `in_write` is accessed atomically. Any report fails the test, because `halt_on_error=1`.
- **ASan/LSan.** Each `rt_cap` is a stack object of about 180 KiB, on the main thread only. The event and buffer allocations are reachable from globals.
- **`build-and-run.sh` needs bash 4** for `declare -A`, and uses `timeout` and `nm` (GNU binutils output format). It is not marked executable in git: run it with `bash`.

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
| `refused_count`, STOP batch not admitted | `p_observe_stop` core:516-520 | `+= k` only if `refused <= 2^63 - k`. Otherwise the count is unchanged and `exhausted` is set. `limit` is set either way. |
| `regerr_count` | `regerr` core:326 | No increment, no REGERR event, exhaust. A REGERR event is attempted only after its increment has been applied, so a REGERR record never outnumbers its counter (format 6). |
| span ids | `zone_begin` core:611 | `next_span_id >= 2^63` exhausts and refuses the begin, which is counted through `count_refused`. Ids handed out are at most 2^63 - 1. |
| refused-begin ids | `zone_begin` core:635 | The begin is counted, the incarnation exhausts, and the handle is INERT. This is F1's "counts nothing" corrected by the boundary rule: the refusal is still counted when that is exact. |
| frame ids | `ps3dk_prof_frame` core:717 | Exhaust and refuse. Unreachable, because a frame id is at most its own seq. |
| sequences | `seq_admit` core:237 | Unchanged format 5.1 rule: refused whole, SEQ_LIMIT. |
| incarnation ids | `thread_begin_impl` core:468 | `next_incarnation = 0` after UINT32_MAX. Later registrations get EFULL and `unregistered_refusals` goes up (5.4). |

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
| `sdk/include/ps3dk/prof.h` | The API, the types, the enabled and disabled macros, the C++ guard, the SPU `#error`. Not in `sdk/Makefile` HEADERS or SUBLIBS: it serves the isolated host tests only. |
| `src/pal/pal.h`, `src/pal/pal_host.c` | The PAL interface and the POSIX implementation. Test faults are injected here. `pal.h` is the shared header every core, drain and PAL object includes, and it refuses with `#error` both PAL macros together, neither, and `PS3DK_PROF_PAL_LV2` with `PS3DK_PROF_TEST_HOOKS` (codex 03:21). Planned preprocess checks, not run: `tests/sdk/ps3dk-prof-probes/pal-select/`. The guard does not prove that every object of a build used the SAME branch: the frozen build request must bind every core, drain and PAL compile invocation and the installed header and provider identities. |
| `src/pal/pal_lv2.c` | The LV2 implementation (TARGET-PLAN-r2 section 1, rulings R1-R8): written for review, NOT built and not in SUBLIBS or any install rule. Hook-free (refuses `PS3DK_PROF_TEST_HOOKS`); needs `-DPS3DK_PROF_PAL_LV2` on every object. Default sink directory `/dev_hdd0/tmp` (accepted, codex 03:21), with exclusive create and the configurable sink kept. NOT every librt return is checked: `pthread_attr_destroy` in `pal_thread_create` is deliberately unchecked (see the note there). The `uintptr_t` low-bit alignment check in `pal_alloc` is accepted; the provider, link-map, alignment and usable-memory gates stay OPEN until a frozen target request. The allocator is `memalign(128, n)` / `free` (C:/wt/prof/PAL-AUDIT-r1.md), replacing `posix_memalign`, which target-c showed is not declared under `-std=c11` and has no libc provider in the stage; `memalign` is declared unconditionally by `sdk/include/stdlib.h` and libc exports it in both ABIs, but link selection and runtime behaviour are not proven by that. The identity call stays `sys_ppu_thread_get_id` -> `sysThreadGetId` (declared in PSL1GHT `lv2/thread.h:23`); its provider is not established: the motivated candidate is the `liblv2_stub.a` alias descriptor (nidgen `liblv2_stub.yaml`, NID 0x350d454e), pending a frozen read-only archive inspection. |
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
| Admission and the 2^63 boundary; a 2-slot BEGIN is refused whole | `seq_admit` core:237; used by `zone_begin` core:615, `emit1` core:303 and `frame` core:713 |
| Whole-publication space check before any write; drop into the pending range | `reserve` core:259 |
| GAP + event as one publication with ONE release; `pub_len` marking (r2 7) | `reserve` writes the GAP; `commit` core:285 |
| Dropping never changes the stack (a dropped BEGIN is pushed and uses its id) | `zone_begin` core:617-628 |
| Refused events assign no sequence and are counted | `count_refused` callers: core:307, 631, 677, 685, 711 |
| STOP batch admitted whole: published, dropped whole, or refused whole; k = 0 does nothing | `p_observe_stop` core:503-540 |

### Format 5.2: depth

| Rule | Code |
|---|---|
| Depth cap 256 | core:609 |
| The END of a refused BEGIN is consumed by the refused-handle LIFO | `zone_end` core:676-681 |

### Format 5.3: descriptor registration

| Rule | Code |
|---|---|
| Check, lock, re-check (Q5) | `desc_index` core:416; `desc_register` core:339 |
| The complete entry, then RELEASE of `desc_count`, then RELEASE of the call-site state | core:393-407, with the hook at core:403 |
| TABLE_FULL / ID_COLLISION: regerr unconditional (ceiling-checked), then a best-effort REGERR, outside the mutex | core:382-390, 411; `regerr` core:322 |
| The failure is cached at the call site; every zone of the site is refused | `desc_index` returns 0 on a failure code; `zone_begin` core:607 |

### Format 5.4: single consumer and drain order

| Rule | Code |
|---|---|
| APP owner identity: a failed read is never the owner (Q1) | `pal_owner_is_self` pal_host.c:46; `ps3dk_prof_drain` drain:627; shutdown drain:688 |
| drain-busy CAS; EBUSY in a drain or in THREAD mode | drain:623-630 |
| Step 1, ring snapshot: table copied under the mutex, ack word ACQUIRE before write-index ACQUIRE | `p_drain_once` drain:355-385 |
| Step 2, tables: THREADS copied under the mutex and encoded after it; DESCRIPTORS up to ACQUIRE of `desc_count` | `declare_threads` drain:247; `declare_descs` drain:270 |
| Step 3, records: whole publications only, `pub_len` validated, chunks of at most 32767 records | `drain_ring` drain:296 |
| Step 4, retirement: RETIRED entry, then the hook, then FREE under the mutex | drain:396-425 |
| Registration refused after the latch; incarnation ids start at 1; exhaustion counted | `thread_begin_impl` core:429 |

### Format 5.5 and 5.6: timebase, frames, tokens

| Rule | Code |
|---|---|
| Timebase read between compiler barriers | `p_tb` core:89 |
| FRAME takes its id with its sequence, then RELEASE of `current_frame` after the publish or the drop; a refused FRAME takes no id | `ps3dk_prof_frame` core:693 |
| Token from an ACQUIRE of `current_frame` | `ps3dk_prof_frame_token` core:732 |

### Format 7: lifecycle

| Rule | Code |
|---|---|
| One capture per process; init EALREADY forever | `gate_enter` core:756; `ps3dk_prof_init` core:1004 |
| Latch RELEASE under `thread_mutex`, fixing Ft; ACQUIRE at every entry point | shutdown drain:694-711; ACQUIRE loads at core:441, 552, 590, 663, 703, 739 |
| Shutdown entry checks in order: in-drain EBUSY, APP non-owner EPERM, once EALREADY | drain:686-691 |
| STOP batch, then the ack as the LAST write (steps 1-5) | `p_observe_stop` core:512-540 |
| Retire: snapshot, clear without synthesising ENDs, then the RETIRED word last; LIVE to RETIRING allowed after the latch (E1) | `ps3dk_prof_thread_end` core:543-572 |
| `thread_end` that observes STOP acks; `thread_end` after the ack is refused | core:551-558 |
| Self-quiescence: the caller's own batch | drain:715; drain thread drain:669 |
| Quiescence wait: bounded, monotonic deadline; a clock failure ends it at once (Q8) | `quiesce_wait` drain:446 |
| Frozen frontier, once, in order 1 to 4; the classification is permanent and comes from the publication (E1) | drain:490-518 |
| No ordinary processing after the frontier (F2) | `p_drain_once` drain:347; nothing frees a slot after `frozen` |
| Final drain to Ft / Fd / F[i]; frozen-RETIRED entries in a RETIRED chunk (F2); the TRAILER | drain:522-594 |
| State-3 entries: UNKNOWN fields, `limit_flags` 0x02, exact `written` | drain:578-582 |
| Storage lifetime: one block, never freed after READY | `init_impl` core:864; `pal_free` only in the init unwind |

### Format 8: sink and result

| Rule | Code |
|---|---|
| Exclusive create; EEXIST; other open errors are ESINKFAIL and retryable (E3) | core:967-976 |
| Short writes retried; 16 tries without progress; EINTR retried and counted as recovered; a count above n, or a negative return, is a SINK FAILURE | `sink_write` drain:22 |
| Nothing written after a failure; trailer only without a failure; flush once after the trailer; close once | drain:586-594; `sink_close` drain:64 |
| Severity order: ESINKFAIL, ECLOSE, ENOTQUIESCED, ESEQLIMIT, ESINKRECOVERED, then ECLOCK, then OK; ETHREAD for a join failure only when otherwise OK | drain:596-608, 737-738 |

### Plan r2 and its errata

| Item | Code |
|---|---|
| r2 3.1 slot states, one consumer, reset in FREE under the mutex | core:464-472 (reset); drain:378-383 and 414-425 (transitions) |
| r2 3.2 + E2 + F1 handle protocol | `zone_end` core:648: step 1 core:657; 2 core:661; 3 core:664; 4 core:666; 5 core:670; REFUSED core:676; PUSHED core:683 |
| F1 identities | Refused ids core:643-644; `prev_refused_top` and `span_top_at_begin` core:604-605 |
| r2 3.3 + E3 + F3 init and gate | `init_impl` core:864: caps core:875-883; checked sizes core:901-908; step 5 core:949-960; step 6 core:962-978; step 7 core:981-990 |
| Setters | core:764, 779 |
| r2 4 atomics | `prof_internal.h` macros. GCC `__atomic` builtins only; lock-free is `_Static_assert`ed. |
| r2 7 / Q9 publication boundaries and caps | `commit` core:285; `drain_ring` drain:296; `take` core:846 |
| r2 8 / Q8 drain thread | `p_drain_main` drain:642: the start gate, one sleep slice, then the wake check |
| Q10 auto-registration | `auto_register` core:502 (opt-in; never for acked or retired TLS; an OS id-read failure gives an INERT zone and consumes nothing, `rt_auto_register_os_id_failure_then_recovery`) |
| Hooks (r2 9 + E1) | `HOOK()` sites: core:297, 403, 536, 558, 567; drain:385, 413, 488 |

### Decisions the contract left open

These choices are recorded for review. They are NOT accepted (codex 2026-10-22 00:59).

| Decision | Choice and reason |
|---|---|
| `set_sink_path` | Names the capture **directory**, so Q7's snprintf-truncation EINVAL is reachable. The file is `<dir>/<app>-<16 hex of the init timebase>.prof`. Documented in `prof.h`. |
| Descriptors the format cannot carry (empty name, a scope other than 1 or 2, invalid UTF-8) | Refused (`DESC_FAILED_INVALID`) and counted as refused. Never written, because the reader would call the file MALFORMED. No REGERR, since there are only codes 1 and 2. Strings are cut to 512 bytes on a UTF-8 boundary, and the id covers the cut strings, as the reader computes it. |
| The first use of a failing site | Also refused (+1). Format 5.1 lists "every zone of a descriptor whose registration failed". |
| A PUSHED END refused at the limit | Still pops: the stack follows the calls. Its span stays OPEN in the records. |
| Zones and frames inside a drain (sink callbacks; the THREAD drain thread) | INERT, with no self-instrumentation. `thread_begin` inside a callback is allowed, which is how the drain thread becomes "registered" (`rt_drain_thread_self_acks`). |
| `unregistered_refusals` | Counts both id exhaustion and "no free slot" (EFULL), and nothing else. The field is informational. An ETHREAD refusal (failed OS thread id read, explicit or auto-registration) is NOT counted, and neither is every zone on a never-registered thread (codex 03:21; format 5.4 wording note). No new wire counter. |
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
| ID_COLLISION (synthetic) | 3 rows | `rt_t_zones.c` |
| R3 OS thread id fails closed (codex 02:45, 03:21) | 3 rows: `rt_thread_begin_os_id_failure_refused`, `rt_thread_begin_os_id_failure_second_thread`, `rt_auto_register_os_id_failure_then_recovery` | `rt_t_lifecycle.c` |
| Off-build and probes (host) | OB-E1, OB-E2 (enabled, undefined, =0; RUN), host OB-P, OB-N, PROBE-G (copy and move), archive symbols | `build-and-run.sh` |

`rt_list.h` is the authoritative list, frozen in sorted form as `tests/host/ps3dk-prof-runtime/rt-registry.txt`, which each variant's `rt --list` must equal exactly: 103 tests, each run under plain, ASan+UBSan and TSan. Every capture is checked by `ps3dk-prof check` against the verdict the test declares.

## 5. Deferred: target-only, needs the cross compiler or LV2

| Test or probe | Why it is deferred |
|---|---|
| `tgt_app_owner_sysThreadGetId`, `tgt_raw_lv2_thread_mutex_tls_identity`, `tgt_sink_eexist_lv2` | LV2 identity, librt mutex and TLS behaviour, and LV2 O_EXCL. These need `pal_lv2.c` and RPCS3 or hardware. |
| OB-P and OB-S (cross `-E` and `-S`, ILP32 and LP64), OB-SPU | Need `powerpc64-ps3-elf-gcc` and `spu-elf-gcc`. The host OB-P and OB-N run here instead. |
| PROBE-A, PROBE-L (both ABIs), PROBE-U, PROBE-O, PROBE-T | Layout, lock-free, libatomic, ordering and timebase codegen on the shipped compilers. PROBE-L's host half is the `_Static_assert` in `prof_internal.h`. |
| PROBE-H on the installed archives | No target archive exists in this step. Its host equivalent, the archive-symbol check, runs on hook-free host objects. |
| OB-H (sdk-header-list) | It runs `make install-headers` and is not run here. `prof.h` is NOT in the sdk/Makefile HEADERS list (codex 00:59): the header serves the isolated host tests only until a library ships. |

**ID_COLLISION: SYNTHETIC branch coverage (codex 00:59 / 01:13 / 02:34; written, not yet built or run).** The path
(core:384-386) is now exercised through a test-only hook. This is branch coverage of the refusal; no natural FNV-1a-64
collision is searched for or claimed.

- **The hook.** `ps3dk_prof_test_hooks.candidate_desc_id` exists only under PS3DK_PROF_TEST_HOOKS and is NULL by default.
  - When set, it replaces ONLY the candidate id that registration compares against the published ids (core:368-373).
  - A new entry stores the natural id (core:402), and the REGERR record carries the natural id, so no descriptor with a
    forced id is ever written.
  - The reader's id validation is not bypassed.
- **`rt_id_collision_detected`:**
  - coll_a is registered normally (the hook returns the natural id).
  - coll_b has a different name and line, and the test checks that its natural id differs from coll_a's. Only its
    candidate id is then forced to coll_a's.
  - Expected: the zone is REFUSED with no BEGIN; `regerr` is 1 and `refused` is 1, and a second use gives cached refused 2
    with no second REGERR.
  - The descriptor count stays 1, coll_a stays in use, and every BEGIN/END names index 0.
  - Exactly one REGERR is written: a=2, b=FNV(name, 0, tag), c=coll_b's natural id. Per format section 4 (record 6),
    c is "the descriptor id it would have had", not a reference to a published descriptor (codex 02:43 Q1).
  - The normal reader verdict is 0 (TRANSPORT COMPLETE).
  - Through the normal `ps3dk-prof report` (section 9: MEASUREMENT VALID needs refused_count=0 AND regerr_count=0;
    `analysis.rs` lane_view): lane 1 is `INVALID (refused 2, regerr 1)`; the statistics scope is `lanes [none]`; no
    zone row is computed for coll_a or coll_b; the one retained descriptor is coll_a exactly as published
    (`index 0 id <coll_a natural id> scope 1 rt_runtime.c:401 coll_a`), and coll_b appears nowhere. So a TRANSPORT
    COMPLETE capture is compatible with this lane being INVALID (codex 02:43 Q2).
  - Not asserted end to end: comparison exclusion. The binary has no compare subcommand; `compare` uses the same
    lane validity (`analysis.rs` "lane invalid" / `InvalidLanesOnly`), but no host test drives it on this capture.
- **`rt_id_noncollision_control`:** the hook stays OFF; both descriptors register (2 descriptors, no REGERR, nothing
  refused). Report: lane 1 MEASUREMENT VALID, scope `lanes [1]`, descriptors 0 (coll_a :401) and 1 (coll_b :402), one
  zone instance each.
- **`rt_id_same_name_not_collision`:**
  - A second static with coll_a's (file, line, name) has its candidate forced to coll_a's id.
  - Expected: REUSE, not a collision. The natural ids are equal, 1 descriptor, no REGERR, and both BEGINs name index 0.
    Report: lane 1 MEASUREMENT VALID, one descriptor with coll_a's natural id, `coll_a: 2 instances`.
- **Report checks** (`REPORT <path> HAS|LACKS|MATCH|NOMATCH <text>` lines, `build-and-run.sh` check_report): the
  normal `ps3dk-prof report` runs once per capture through rrun (exit 0 or 1 required), and its lines with leading
  spaces removed are matched exactly (grep -Fx) or as a whole-line ERE (grep -Ex). The report now prints a
  `descriptors (as published):` section (tools/ps3dk-prof report.rs; test
  `report_lists_every_published_descriptor_with_its_identity`; Cargo.lock unchanged).
- **Hook-free checks:** the archive check (no ps3dk_prof_test_ symbol), plus an off-build preprocessor check (the
  hook-free preprocessed prof_core.c names neither candidate_desc_id nor ps3dk_prof_test_hooks). Both use grep_status:
  status 0 = found (FAIL), 1 = absent (PASS), >1 = inspection failure (FAIL), never read as absent (codex 02:43).
- **Counts:** 100 registry tests x 3 variants = 300 rows; 29 non-row checks (driver-tsan selftest, 3 variant builds,
  TSan controls, the hook-free library build, 4 archive/preprocessor checks, 16 OB-E1/E2 checks, 3 PROBE-G), so a
  fully passing run reports pass 329 (build-m at f01e1bcd: 291 + 28 = 319). With the two R3 rows (PAL work on top of
  157c42ce) the expectation became 102 x 3 = 306 rows and pass 335, and with the auto-registration recovery row (codex
  03:21) 103 x 3 = 309 rows and pass 338 (build-n measured 300 + 29 = 329 at 157c42ce); expectations, not
  measurements.
- **Still open:** the full runtime code review, and these tests' first host run (a separate request).

## 6. Expected compile risks

- **`PS3DK_PROF_ZONE_BEGIN` statement expressions** (`__extension__({...})` with a function-local static) in C++17 with `-Wall -Wextra -Werror`. GCC and clang accept them; `-Wpedantic` is not used.
- **clang `-Wunevaluated-expression`** on the off-build `sizeof(zs[1 - hits++])`. The script adds `-Wno-unevaluated-expression` for clang only.
- **`-Wunused-but-set-variable`** on `zs` and `tok` in the disabled OB build. GCC marks sizeof operands as read. clang 13 or later probably does too, but this has not been checked.
- **`_Static_assert` and `_Thread_local`** are only in the C core and C tests. The C++ TUs include `rt.h` and `prof.h`, which use neither.
- **`posix_memalign` and `nanosleep` under `_POSIX_C_SOURCE 200809L`** in `pal_host.c`; `CLOCK_REALTIME` and `pthread_cond_timedwait` in `rt_util.c`. The core files define no feature macro and include `<pthread.h>` under strict `-std=c11`.
- **`(uint64_t)(uintptr_t)pthread_self()`** works on glibc, where `pthread_t` is an integer. On a libc where `pthread_t` is a struct it would not compile; only Linux CI is the target.
- **`snprintf` `-Wformat-truncation`** on the sink name (core:967). Its return value is checked, so level 1 should stay quiet.
- **TSan.** Tests change hook pointers while other threads run, but only on fields those threads do not read. The memory sink's `in_write` is accessed atomically. Any report fails the test, because `halt_on_error=1`.
- **ASan/LSan.** Each `rt_cap` is a stack object of about 180 KiB, on the main thread only. The event and buffer allocations are reachable from globals.
- **`build-and-run.sh` needs bash 4** for `declare -A`, and uses `timeout` and `nm` (GNU binutils output format). It is not marked executable in git: run it with `bash`.

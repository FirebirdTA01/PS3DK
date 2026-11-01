# Automatic profiler lifecycle contract

This records the automatic runtime's integration requirements. The optional
thread-exit notification and automatic startup/finalization object are implemented.
The SDK builds and installs separate manual and automatic archives for both
ABIs; the target lifecycle tests link those installed archives. Compiler
adapters, the one-switch demonstration and release packaging remain separate work.
The manual runtime and its v1 capture validity rules remain unchanged.

## Thread retirement boundary

`librt` calls the private weak `__ps3dk_prof_thread_exit` notification after
`thread_local` destruction and all pthread-key destructor rounds, outside the
thread-table lock and before marking or releasing the exiting thread's slot.
There is no declaration in a public header. An unprofiled application leaves it
undefined; both ILP32 and LP64 must check the weak address without dereferencing
a null function descriptor. A weak reference alone must not extract the
automatic archive; that archive will provide a forced startup anchor.

The existing cleanup reentry guard is set before any destructor or notification.
Failure to adopt a slot created by TLS cleanup, or to lock the table during key
destruction, suppresses the notification: skipped cleanup cannot acknowledge a
producer as retired.
If a destructor exits recursively, remaining cleanup and the notification are
abandoned. If the notification itself exits recursively, it is not called again.
The eventual profiler callback must return normally, retire only its existing
producer, and never allocate or register a replacement producer. `pthread_cancel`
returns `ENOSYS`; a refused cancellation is not a retirement event.

`tests/regression/profiler-thread-exit` links the installed `librt.a`
and runs with the hook defined and undefined on both ABIs. Its four manifest
rows compile no runtime source and inject no faults. It covers two rounds
of key destruction, pthread and Cell returns, their explicit exits, cancellation
refusal, raw exit, destructor reentry, notification reentry and a slotless Cell
worker (64 checks per row). The optional CMake setting
`PROFILER_EXIT_CANDIDATE_FAULT_TESTS=ON` adds four `-candidate-fault` targets
with a copied `pthread.c` and test-only mutex interposition. Those targets add
lock-failure cases (78 checks per row); they are candidate-only tests run before
installation and are deliberately absent from the regression manifest. Neither
group validates a functioning automatic profiler.

## Exit routes and required capture verdicts

“Complete” below requires a valid final trailer, balanced recorded spans, no
loss/refusal and successful transport. Retirement alone never certifies timings.
These are requirements for the remaining automatic integration, not hardware
support claims.

| Exit route | Cleanup notification | Required capture outcome |
|---|---|---|
| Return from a pthread entry | After TLS and all key rounds | Eligible for complete; open spans or other losses remain visible |
| `pthread_exit` mid-function | After TLS and all key rounds | Incomplete/censored if the non-returning call leaves an instrumented span open |
| Return from a non-interrupt Cell `sys_ppu_thread_create` entry | Current SDK trampoline performs cleanup | Eligible for complete under the same conditions as pthread return |
| Cell `sys_ppu_thread_exit` through the SDK wrapper mid-function | SDK wrapper performs cleanup | Incomplete/censored for unreturned instrumented spans; this is a covered cleanup route |
| Raw `sysThreadExit` or direct exit syscall | Bypasses cleanup | Unretired producer, visibly incomplete; joining it does not fabricate an acknowledgement |
| Interrupt thread, or code built against older raw inline Cell wrappers | No current cleanup-trampoline guarantee | Visibly incomplete if a registered producer does not acknowledge shutdown |
| Destructor calls an exit route recursively | Remaining cleanup abandoned | Visibly incomplete; no normal retirement notification |
| Normal return from main | Finalization after user destructors | Eligible for complete only after the final drain |
| `exit` inside instrumented code | Normal process cleanup, but no return from that frame | Incomplete/censored for open spans even if a final trailer is written |
| `abort`, `_exit`, raw process-exit syscall | No guaranteed final drain | Missing trailer/torn tail is incomplete; no artifact means no capture, not a zero-work success |
| `quick_exit` | Does not run the normal fini path | Incomplete/no capture; no normal-finalization claim |
| sysutil exit-game callback | Depends on the application's exit path | Normal main return may complete; `exit`, raw process exit or abandoned workers follow the corresponding rows above |

The existing format expresses an unacknowledged producer with a `NOT_QUIESCED`
entry and `end_flags` bit0 at the shutdown frontier. Missing final drainage
produces `NoTrailer` and/or `TornTail` in the host reader. Open spans retain the
format's explicit validity/censoring behavior. The automatic integration must
exercise these states through actual capture/reader tests; a documentation
warning or a successful join cannot replace them.

## Startup and finalization requirements

Internal constructor priority100 follows the libc runtime ladder (heap83,
syscall table84, metaLock85, finalization86) and libstdc++ priority90/98,
before ordinary user constructors at101. Libc locks are already live; newlib
patch0004 also retains thread-safe lazy creation for earlier callers.
The priority101 allocation/capture regression runs on both ABIs. The drain
thread exists before user constructors and main and accesses only PAL and
lock-safe libc/runtime paths. It does not invoke application code.

Finalization at destructor100 follows ordinary user destructors. An `atexit`
callback registered at startup is too early for user attribute destructors.
libstdc++ priority90/98 destructors run later; keep libstdc++, CRT, profiler,
platform layer and transport dependencies uninstrumented, with a link-map and
compile-flag exclusion check. Write capture data through filesystem syscalls,
not stdio teardown assumptions.

The automatic object uses the runtime defaults (64 producer slots, 1024
descriptors, 4096 records per ring, 10 ms drain interval, 1 s quiescence limit),
with thread mode and first-use registration enabled. The file sink defaults to
`/dev_hdd0/tmp/app-<timebase>.prof` on LV2 with exclusive creation. Initialization
and finalization failures emit an unbuffered diagnostic; missing artifacts,
trailers and unacknowledged producers retain the existing reader verdicts.
An open span is censored with unknown duration; it does not alone invalidate
an otherwise valid lane or imply transport failure.

`tests/host/ps3dk-prof-runtime/auto-lifecycle-test.py` exercises real captures
through the production reader, including abnormal exits, unretired producers,
initialization failure and archive linkage without the startup anchor. It runs
from the existing host driver. Host cleanup explicitly models the SDK's final
notification; `tests/sdk/ps3dk-prof-auto` instead uses the actual linked SDK
notification after TLS destruction and two pthread-key rounds. These fixtures
use manual zones to test lifecycle, not dummy compiler hooks. The target row
compiles only the fixture and links installed `libps3prof.a` through the forced
`__ps3dk_prof_startup_anchor` symbol, verifying extraction of its init/fini
entries. `PROFILER_FORCE_STARTUP=OFF` is the negative control: the same fixture
links, runs and produces no capture. Both controls link with `--gc-sections`.
Run both settings with `PROFILER_ABI=ilp32`
and `PROFILER_ABI=lp64`, preserving each link map and capture-reader result.

The `sdk/libps3dkprof/Makefile` install target is included by the top-level SDK
install. It writes `libps3dkprof.a` (manual core/PAL, no startup) and
`libps3prof.a` (core/PAL, startup and arc serializer) to `ppu/lib` and
`ppu/lib/lp64`. The top-level SDK `HEADERS` list alone owns installation of
`<ps3dk/prof.h>`. Build products live below
`PS3_BUILD_ROOT/sdk/libps3dkprof`; neither archive inherits application
`CFLAGS`/`CPPFLAGS` or includes compiler instrumentation. The automatic archive
does not yet contain the Task4 compiler adapters.

To validate an install into a private SDK prefix, run
`python3 tests/sdk/ps3dk-prof-auto/install-test.py <new-evidence-dir> --sdk <private-sdk> --cross <ppu-tool-prefix>`.
This runs the top-level header install and sublibrary install, checking archive
membership, startup-anchor ownership, isolation from application instrumentation
flags and exact installed public-header bytes. It complements, rather than
replaces, the target link/runtime controls above.

Do not demonstrate `PS3_PROFILE=ON` using dummy compiler hooks or an empty
capture. The unchanged-source counts and timings gate requires the real
`_mcount` and `__cyg_profile_func_enter/exit` adapters. RPCS3 comes first;
physical HEN capture/count/overhead and DEX context-sampling checks are distinct
later acceptance gates.

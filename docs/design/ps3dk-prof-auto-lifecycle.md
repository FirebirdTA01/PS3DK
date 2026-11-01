# Automatic profiler lifecycle contract

This records the automatic runtime's integration requirements. The optional
thread-exit notification is implemented; automatic startup, compiler adapters,
capture finalization and the one-switch demonstration remain separate work.
The manual runtime and its v1 capture format are developed separately on the
profiler branch; this notification does not install or enable that runtime.

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

The profiler branch's v1 format expresses an unacknowledged producer with a `NOT_QUIESCED`
entry and `end_flags` bit0 at the shutdown frontier. Missing final drainage
produces `NoTrailer` and/or `TornTail` in the host reader. Open spans retain the
format's explicit validity/censoring behavior. The automatic integration must
exercise these states through actual capture/reader tests; a documentation
warning or a successful join cannot replace them.

## Startup and finalization requirements

Use internal constructor priority100 after the SDK heap/syscall setup and the
libstdc++ priority90/98 constructors, before ordinary user constructors. This
precedes newlib's priority105 lock constructor: allocating or taking a libc
lock therefore relies on the lazy metaLock fix in newlib patch0004 (b7327b5c).
Keep an early-allocation/lock regression on both ABIs when adding startup.

Finalization at destructor100 follows ordinary user destructors. An `atexit`
callback registered at startup is too early for user attribute destructors.
libstdc++ priority90/98 destructors run later; keep libstdc++, CRT, profiler,
platform layer and transport dependencies uninstrumented, with a link-map and
compile-flag exclusion check. Write capture data through filesystem syscalls,
not stdio teardown assumptions.

Do not demonstrate `PS3_PROFILE=ON` using dummy compiler hooks or an empty
capture. The unchanged-source counts and timings gate requires the real
`_mcount` and `__cyg_profile_func_enter/exit` adapters. RPCS3 comes first;
physical HEN capture/count/overhead and DEX context-sampling checks are distinct
later acceptance gates.

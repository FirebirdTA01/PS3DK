# POSIX threads on the PS3 toolchain — what the librt shim does and does not do

The reference an SDK user needs, plus the semantics that will bite whoever
ports the next library. Nothing here is a design document.

Source of truth: `runtime/lv2/librt/pthread.c` and `sdk/include/pthread.h`.

## 1. Why it exists

Third-party libraries do not ask "does this platform have threads?" — they run an
autoconf or CMake probe that compiles and links a small POSIX program. Before this shim
that probe failed on our target, and the libraries quietly dropped to their no-threads
tier: cairo to `-DCAIRO_NO_MUTEX`, pixman to `-DPIXMAN_NO_TLS`. Those are not "threading
disabled" switches — they turn the library's internal caches into *unguarded shared
globals*, which is a data race the moment two threads touch them. The shim removes the
need for that fallback, and for the per-library patches that were the alternative.

The probe failed for a specific reason, measured rather than assumed:

> newlib guards the **entire body** of its `<pthread.h>` behind `_POSIX_THREADS`, and
> `<sys/features.h>` defines that macro only for `__rtems__`. On this target
> `#include <pthread.h>` therefore yields *nothing at all* — no prototypes, and no
> `PTHREAD_MUTEX_INITIALIZER` / `PTHREAD_COND_INITIALIZER` / `PTHREAD_ONCE_INIT`, because
> those three are `#define`d inside the skipped body.
>
> The underlying `_PTHREAD_MUTEX_INITIALIZER` family **is** defined, in
> `<sys/_pthreadtypes.h>` — as are `pthread_mutex_t` and the rest of the types. Only the
> public layer is missing. (An earlier version of the plan had this the other way round.)

## 2. What you get

`#include <pthread.h>` and link normally. No flags, no `-L`, no `-lpthread` even —
the compiler driver's own link group already carries `-lrt`, which is where the
implementation lives. `-lpthread` is nevertheless honoured, because the SDK installs a
`libpthread.a` linker script (`INPUT(-lrt)`) on the default `-l` search path, so autoconf
packages that hardcode it detect us with no recipe help. Both ABIs (ILP32 and LP64).

Implemented, all with POSIX errno returns:

| Area | Functions |
|---|---|
| Mutexes | `init` `destroy` `lock` `trylock` `timedlock` `unlock` |
| Mutex attributes | `init` `destroy` `settype` `gettype` `setpshared` `getpshared` |
| Conditions | `init` `destroy` `wait` `timedwait` `signal` `broadcast` |
| Condition attributes | `init` `destroy` `setclock` `getclock` `setpshared` `getpshared` |
| Once | `pthread_once` |
| Thread-specific data | `key_create` `key_delete` `setspecific` `getspecific` |
| Threads | `create` `join` `detach` `exit` `self` `equal` `yield`, `sched_yield` |
| Thread attributes | `init` `destroy` `set/getdetachstate` `set/getstacksize` `set/getstack` `set/getstackaddr` `set/getguardsize` `set/getschedparam` |
| Cleanup | `pthread_cleanup_push` / `pop` (and the `_defer` / `_restore` GNU spellings) |
| Accepted no-ops | `pthread_atfork`, `setcancelstate`, `setcanceltype`, `testcancel` |

Mapping: mutexes onto `sysMutex*` (Lv-2 syscalls 100–104), conditions onto `sysCond*`
(105–109), threads onto `sysThread*` (43–49 plus the sysPrxForUser entries).

## 3. Semantics that differ from a hosted POSIX — read this before porting

**Static initialisers are lazy.** An Lv-2 mutex id is not a compile-time constant, it
comes back from syscall 100. So `PTHREAD_MUTEX_INITIALIZER` stores a sentinel
(`0xFFFFFFFF`; `0xFFFFFFFE` for `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`) and the kernel
object is created on first use, under one process-wide lock. Consequences: the first
`pthread_mutex_lock` on a static mutex can fail with `ENOMEM`, and a zeroed
`pthread_mutex_t` (from `calloc`, or `memset`) is deliberately treated as "not created
yet" rather than as undefined behaviour.

**Conditions bind to their mutex on first wait, not at init.** `sysCondCreate` takes the
mutex as a creation argument, while POSIX names it only at `pthread_cond_wait`. So
`pthread_cond_init` records a sentinel and creation is deferred to the first wait.
`pthread_cond_signal` / `broadcast` on a never-waited-on condition return 0 without doing
anything — which is exactly right, since there can be no waiters. Using one condition
with two different mutexes is undefined in POSIX; here the first mutex wins silently.

**`PTHREAD_MUTEX_ERRORCHECK` behaves as `PTHREAD_MUTEX_NORMAL`.** The Lv-2 primitive is
recursive or it is not; there is no relock-detecting mode in between.
`PTHREAD_MUTEX_RECURSIVE` is fully supported (`SYS_MUTEX_ATTR_RECURSIVE`), which is what
cairo's `CAIRO_MUTEX_HAS_RECURSIVE_IMPL` needs.

**Relocking a mutex you already hold: `lock` reports, `trylock` says EBUSY.** Lv-2 splits
the already-locked case that POSIX treats as one — `lv2_mutex::try_lock` (RPCS3
`Emu/Cell/lv2/sys_mutex.h:82-98`) returns `CELL_EBUSY` when *another* thread holds the
mutex but `CELL_EDEADLK` when the *calling* thread holds a non-recursive one. POSIX makes
no such distinction for `pthread_mutex_trylock`: it fails with `EBUSY` because the mutex
could not be acquired, whoever holds it. So the shim folds the deadlock status into
`EBUSY` **in `trylock` only**. `pthread_mutex_lock` deliberately keeps returning `EDEADLK`
instead of blocking forever, which is a divergence from `PTHREAD_MUTEX_NORMAL` (where
self-relock is undefined behaviour that usually hangs) and a strict improvement on it.

**No cancellation.** `pthread_cancel` returns `ENOSYS`. `pthread_setcancelstate` and
`setcanceltype` succeed and remember nothing, because library code routinely brackets
critical sections with them and only checks the result in debug builds. Cleanup handlers
run when popped with a non-zero argument and at no other time — in particular
`pthread_exit` does **not** unwind them.

**`pthread_once` chains are serialised process-wide.** All one-time initialisers share a
single recursive mutex, so two threads running *different, unrelated* `pthread_once`
controls block on each other. Recursion is fine (a once-routine may call `pthread_once`
again, on the same or another control), and so is a once-routine that creates mutexes —
that takes the separate table lock. But a once-routine that **waits on another thread
which is itself inside a `pthread_once`** will deadlock, where a per-control implementation
would not. Nothing we ship does this; splitting the lock per control is the fix if
something ever needs it.

**Bounded tables.** 32 threads may hold thread-specific data at once and 32 keys may
exist at once (`PT_THREADS_MAX` / `PT_KEYS_MAX` in `pthread.c`). Exhaustion is reported as
`EAGAIN`, never ignored. A thread that was not created through `pthread_create` — the main
thread, or one from a raw `sys_ppu_thread_create` — is registered on first use and its slot
is released only by `pthread_exit`; a foreign thread that just returns leaks its slot.

**`pthread_t` is an index, not an Lv-2 id.** `sys_ppu_thread_t` is 64 bits and newlib's
`pthread_t` is 32, so the raw id cannot be stored. A `pthread_t` is a 1-based index into
the thread table and 0 is always invalid.

**Stacks.** `sys_ppu_thread_create` always allocates its own stack, so
`pthread_attr_setstackaddr` is remembered for round-tripping but not honoured.
`setstacksize` is honoured; the default is 64 KiB (the PSL1GHT samples' 0x1000 is far too
small for POSIX-shaped code). Default priority is 1000, the same band as the main thread.

**`pthread_mutex_trylock` returns POSIX `EBUSY` (16), not the Lv-2 value.**
`<sys/synchronization.h>` redefines `EBUSY` to `0x8001000A` for source compatibility with
Sony's samples; `pthread.c` deliberately does **not** include that header, and translates
Lv-2 status through `lv2error()`. If your translation unit includes
`<sys/synchronization.h>`, your own `EBUSY` comparisons will not match what the shim
returns.

## 4. Two things the header does on purpose

The shadow `sdk/include/pthread.h` turns `_POSIX_THREADS` on **only across the
`#include_next`** and back off afterwards, so the prototypes come from newlib verbatim and
cannot drift. Leaving it defined would add two members to `struct sigevent` in
`<sys/signal.h>`, giving translation units that include `<pthread.h>` a different layout
from those that do not.

`_UNIX98_THREAD_MUTEX_ATTRIBUTES` is left **off** for the same class of reason: it appends
a `type` member to `pthread_mutexattr_t`, and `<sys/_pthreadtypes.h>` is pulled in by
`<sys/types.h>` almost everywhere, so the struct would have two layouts in one program.
`PTHREAD_MUTEX_RECURSIVE` and `pthread_mutexattr_settype` / `gettype` are declared in the
shadow instead, and the shim stores the kind in the `recursive` member, which exists in
both layouts.

**Known limitation of the shadow:** under a strict-ISO dialect (`-std=c99`, `-std=c11`)
`__POSIX_VISIBLE` drops below 199506, so a `<sys/types.h>` included *before* `<pthread.h>`
consumes `<sys/_pthreadtypes.h>` without defining the pthread types, and no later include
can recover them. The shadow detects exactly that case and emits one clear `#error`
naming the fix (`-std=gnu99` or later) instead of a hundred unknown-type errors.

## 5. Verification performed

- **RED first.** `probe-cairo` / `probe-pixman` / `probe-cond` were written before any
  implementation and all three failed to compile, with `PTHREAD_MUTEX_INITIALIZER
  undeclared` and implicit-declaration warnings for every entry point.
- **Strict compile.** `-Wall -Wextra -Werror`, clean, ILP32 and LP64.
- **nm gate.** All 60 expected symbols defined in `librt.a` for both ABIs. `pthread.o`'s
  only undefined references are `.TOC.`, `lv2error`, `memset`, `sysThreadCreate`,
  `sysThreadExit`, `sysThreadGetId` — every one resolvable from the driver's default
  `--start-group`, so no link-line change is needed anywhere.
- **Probe gate, 18/18**, run against both the WSL stage prefix and the installed Windows
  SDK: 3 probes × 2 ABIs × `LIBS` in {`-lpthread`, `-lrt`, nothing}, all rc=0.
- **Boot gate.** `PTHREAD_OK` from `samples/lv2/hello-ppu-pthread` under desktop RPCS3;
  the same program runs in the regression battery as the `pthread-sync` row.

Note for whoever reads an OLD probe log (pre patch 0035): every link with those
toolchains — including `int main(void){return 0;}` — printed `ld: error in
.../crtend.o(.eh_frame); no .eh_frame_hdr table will be created` on stderr. The root
cause was `crtend.o` carrying a real FDE *after* the `.eh_frame` zero terminator, which
ld's unconditional `.eh_frame` editing parses into and chokes on. Fixed by
`patches/ppu/gcc-12.x/0035-libgcc-t-ps3-crtstuff-no-unwind-tables.patch` (crtstuff built
`-fno-asynchronous-unwind-tables`, so crtend's `.eh_frame` is exactly the 4-byte
terminator); `scripts/check-release-tree.sh` gates the section size so it cannot ship
malformed again. Links are silent now, and cairo's stderr-sensitive pthread probe passes
with no filter.

## 6. Follow-ups this leaves open

1. ~~The `crtend.o` `.eh_frame` complaint~~ — RESOLVED by GCC patch 0035 (crtstuff
   built `-fno-asynchronous-unwind-tables`); the `040-cairo.sh` filter is removed and
   `check-release-tree.sh` gates the crtend section size.
2. pixman rides the `__thread` path, so **cairo is the only portlib that exercises the
   shim**; pixman's `HAVE_PTHREADS` arm stays unexercised. Keep `hello-ppu-pthread` in the
   battery as the direct coverage for `once` / `key` / `getspecific`.
3. TLS in PRX modules (prx-gen refuses `.tdata`) and new-thread TLS on real hardware are
   still open from the threading plan — they bound how far the `__thread` route can go,
   and the pthread-key path is the fallback if either fails.
4. Per-thread newlib reent: threads get their locking from the reent patches, measured OK
   for `malloc` under two threads, but not audited beyond that.
5. `pthread_condattr_setclock` refuses everything but `CLOCK_REALTIME`, because
   `sysCondWait` takes a relative timeout that `pt_timeout_usec` derives from the wall
   clock. A port that asks for `CLOCK_MONOTONIC` is not unreasonable and would want the
   deadline computed against the timebase instead — revisit when one does.
6. `pthread_once` serialises across unrelated controls (see §3). Per-control locking if
   anything ever needs it.

## 7. A trap worth knowing about, found the hard way

The first `hello-ppu-pthread` deadlocked under RPCS3, and the shim looked like the obvious
suspect. It was not. The sample used **one condition variable for two different
predicates** — main waiting for `ready_workers == 2`, the workers waiting for
`release_workers` — and woke it with `pthread_cond_signal`. POSIX gives no way to aim a
signal at a predicate class, so a worker's signal can legally wake another *worker*, which
re-checks its own predicate, finds it false, and sleeps again; the wakeup main needed is
consumed and everyone parks.

Two things made this cheap to settle rather than expensive to argue about. The syscall
frequency dump was decisive on its own: `sys_cond_wait` 4, `sys_cond_signal` 1,
`sys_cond_broadcast` 0, and `sys_mutex_lock` minus `sys_mutex_unlock` equal to 3 — which
is exactly three threads parked inside `sys_cond_wait`, each of which entered holding the
mutex and released it internally rather than through the syscall. And rebuilding the same
structure as a host program proved it had nothing to do with this platform: on glibc 2.39
it deadlocks 5 runs out of 5 with `signal` and passes 5 out of 5 with `broadcast` — an
experiment reproducible with nothing but a hosted C compiler: same two-predicate
structure, force a worker to take the mutex first. **When a port hangs on a condition variable,
count the waits and signals in the log and reproduce it on the host before suspecting the
shim.**

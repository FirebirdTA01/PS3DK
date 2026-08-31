# host-repro: is it the shim, or is it the sample?

`hello-ppu-pthread` uses one condition variable for two predicates:
`main` waits for `ready_workers == NWORKERS`, and the workers wait for
`release_workers`. Both waiter populations are therefore parked on
`shared_cond` at the same time.

`pthread_cond_signal` wakes one arbitrary waiter of a condition variable. POSIX
does not provide a way to aim it at a predicate class. A worker's "I am ready"
signal may legally wake another worker, which re-checks its own predicate, finds
it false, and goes back to sleep after consuming the wakeup that `main` needed.
Everyone parks. That is why the sample calls `pthread_cond_broadcast`, not
`pthread_cond_signal`, after incrementing `ready_workers`.

Do not simplify that broadcast back to a signal. It deadlocks, and it deadlocks
rarely enough to look like a flaky emulator or toolchain bug. This exact shape
caused the red `pthread-sync` regression row in RPCS3 run
`run-20260830-161359`: one worker's ready signal was consumed by the other worker
instead of by `main`.

## What this test is for

`hostrepro.c` is the same wait/signal structure, on the host, with the relevant
interleaving forced by two `usleep` calls. It needs only a host C compiler and
pthreads: no PS3 toolchain, no emulator, no librt shim.

```sh
./hostrepro.sh
```

The script builds and runs two variants five times each:

- `signal`: expected to deadlock and time out with exit code 124.
- `broadcast`: expected to pass with exit code 0 and print `HOST_OK`.

If the `signal` build hangs on your host too, the bug is in the sample's
synchronization pattern, not in the librt pthread shim. Measured on glibc 2.39
(x86-64): 5 runs out of 5 deadlock with `signal`, and 5 out of 5 pass with
`broadcast`.

## Reading a target hang

The RPCS3 syscall frequency dump can identify this bug without changing code.
Two counts matter:

- `sys_cond_wait` vs `sys_cond_signal` vs `sys_cond_broadcast`. Wakeups fewer
  than the waiters that needed them are the lost-wakeup signature.
- `sys_mutex_lock` minus `sys_mutex_unlock` equals the number of threads
  currently parked inside `sys_cond_wait`: each entered holding the mutex and
  released it internally, not through the unlock syscall.

In `run-20260830-161359`, that difference was exactly 3, matching the three
parked threads and making the diagnosis deterministic rather than plausible.

# PPU process finalization

Returning from `main` calls newlib `exit`. Normal `exit` runs registered
termination callbacks and flushes stdio before reaching the Lv-2 process
termination hook. `_exit` and the termination path used by `abort` bypass
those callbacks. The low-level `__librt_exit` hook calls `sysProcessExit`
directly; it must not run `_fini`.

CRT constructor 106 registers `_fini` with `atexit`, after heap initialization
(103), syscall-table initialization (104), and newlib recursive-lock
initialization (105). Application registrations follow that callback and
run before it during normal termination. C++ destructor ordering also
depends on how the target compiler registers destructors; inspect the PPU
probe's calls rather than assuming the host compiler's model applies.

The malloc arena remains mapped throughout finalization. Other PPU threads
may still use it; a `.fini` callback cannot safely free it. Lv-2 reclaims
the process-owned memory when the process terminates. This does not add a
thread-joining policy, change explicit application memory frees, or affect
the SPU runtime.

`tests/sdk/librt-exit-test.sh` checks low-level termination and normal-exit
callback registration with a mocked Lv-2 boundary. The target probe
`tests/sdk/exit-runtime.cpp` has four build modes: return from `main`,
`exit`, `_exit`, and `abort`. For normal termination it checks the order
of C++ destructors interleaved with `atexit` registrations and the flushing
of buffered output without a newline. Target execution requires a
coordinated emulator or hardware slot.

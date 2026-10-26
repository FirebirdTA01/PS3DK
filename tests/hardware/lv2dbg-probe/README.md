# lv2dbg hardware probe

Phase 1 of the debugger plan ([docs/design/debugger-design.md](../../../docs/design/debugger-design.md)).
An in-app debug stub needs four lv2dbg services: thread ids, reading another
thread's register context, PPU exception-handler registration and the DABR
data breakpoint. This probe asks whether each one answers on the console it
runs on, under HEN on a retail console in particular. It is not a CI test:
it is built and run by hand.

## What it does

1. Records its own thread id, then loads `CELL_SYSMODULE_LV2DBG`.
2. `sys_dbg_get_ppu_thread_ids`, before and after starting a worker thread
   that sleeps in a loop (the probe waits up to 2 s for it to report ready).
3. `sys_dbg_read_ppu_thread_context` on the worker and on itself; logs pc,
   lr, r1 and r2.
4. `sys_dbg_get_ppu_thread_name` and `sys_dbg_get_ppu_thread_status` for the
   worker.
5. Initializes, registers, unregisters and finalizes a PPU exception
   handler. No exception is raised, so the handler is not expected to run.
6. Reads the DABR; if it read and no watch is active, arms a write watch on
   a variable nothing touches, reads it back, restores the prior state and
   reads it again.
7. Stops and joins the worker, and unloads the module if the probe loaded
   it.

Every lv2dbg call is preceded by a `CALL <name>` line that is written and
flushed to the log before the call is made, and followed by
`RC <name> 0x...`; values follow as `VAL` lines, skipped steps as
`SKIP <name>: <reason>`. A hang therefore names its call. If a `CALL` line
cannot be made durable, the probe makes no further observation call. Cleanup
steps (unregister, finalize, DABR restore, thread join, module unload) are
attempted even when their `CALL` line could not be made durable, and their
results are checked: the DABR is restored to its exact
prior state and read back, and the module is unloaded only if the probe
loaded it. A watch that is already active is left alone.

Output goes to stdout and to a log in `/dev_hdd0/tmp`. An existing log is
never overwritten: the probe uses the first free name of
`lv2dbg-probe.log`, `lv2dbg-probe-1.log`, and so on. The last log line is
`LV2DBG_PROBE_END status=0x..`; the last stdout line,
`LV2DBG_PROBE_DONE status=0x.. log=ok|failed`, is written after the log is
closed.

The exit status is a set of bits. 0 means the observation sequence
completed, with any steps skipped for a missing dependency recorded as
`SKIP` lines; it does not mean every step ran. In RPCS3, for example, the
exception-handler register, unregister and finalize steps are skipped,
because initialization is refused.

| Bit | Meaning |
|---|---|
| 0x02 | `CELL_SYSMODULE_LV2DBG` did not load; no lv2dbg call is made |
| 0x04 | the worker thread could not be created or did not become ready within 2 s |
| 0x08 | the log could not be created, written, flushed or closed |
| 0x10 | cleanup incomplete: a failed unregister, finalize, DABR restore, join or unload |

A return code from an lv2dbg call that the console does not provide is an
observation, not a failure.

## Building

Configure with the SDK toolchain file. It builds `lv2dbg-probe-ilp32` and
`lv2dbg-probe-lp64`.

For the network loader (`ps3dk-run`), the probe must link the protocol-2
run hook, or the loader never hears from it. Pass
`-DPS3DK_RUN_HOOK_DIR=<dir>`, where `<dir>` holds `hook/` and `common/` from
the `spike/ps3dk-run` tree at the protocol the installed loader speaks.
Without that option the probe is a plain SELF for RPCS3 or an XMB launch.

## Failure-path simulations

`-DLV2DBG_PROBE_SIM=ON` also builds `lv2dbg-probe-sim-<case>-ilp32`, which
replace the lv2dbg calls or the log with simulations, for RPCS3 only:
`ok` (every lv2dbg call succeeds), `log-open` (the log cannot be created),
`log-write` (log writes fail after six lines), `dabr-active` (a watch is
already set), `cleanup-fail` (unregister, finalize and the DABR restore
fail) and `log-exists` (an earlier log already holds the first name; it
must be left intact).

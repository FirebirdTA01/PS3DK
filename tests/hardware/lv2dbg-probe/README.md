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
   that sleeps in a loop.
3. `sys_dbg_read_ppu_thread_context` on the worker and on itself; logs pc,
   lr, r1 and r2.
4. `sys_dbg_get_ppu_thread_name` and `sys_dbg_get_ppu_thread_status` for the
   worker.
5. Initializes, registers, unregisters and finalizes a PPU exception
   handler. No exception is raised, so the handler is not expected to run.
6. Reads the DABR, arms it as a write watch on a variable nothing touches,
   reads it back, and clears it.
7. Stops and joins the worker, and unloads the module.

Every call is preceded by a flushed `CALL <name>` line and followed by
`RC <name> 0x...`; values follow as `VAL` lines. A hang therefore names
its call. Output goes to stdout and to `/dev_hdd0/tmp/lv2dbg-probe.log`.
The last line is `LV2DBG_PROBE_DONE status=<n>`.

Exit status: 0, every step ran (whatever the return codes); 2, the module
did not load and no lv2dbg call was made; 3, the worker could not be
created and the context steps were skipped.

## Building

Configure with the SDK toolchain file. It builds `lv2dbg-probe-ilp32` and
`lv2dbg-probe-lp64`.

For the network loader (`ps3dk-run`), the probe must link the protocol-2
run hook, or the loader never hears from it. Pass
`-DPS3DK_RUN_HOOK_DIR=<dir>`, where `<dir>` holds `hook/` and `common/` from
the `spike/ps3dk-run` tree at the protocol the installed loader speaks.
Without that option the probe is a plain SELF for RPCS3 or an XMB launch.

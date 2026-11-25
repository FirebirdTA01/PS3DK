/*
 * ps3dk-run: run hook linked into a child SELF.
 *
 * The child calls ps3dk_run_hook_start(&argc, &argv) as the first statement of main (tests/hardware/psgl-sample-boot/
 * run-hook-inject.cmake adds such a main to an unmodified sample).  Output written by
 * constructors that ran before main is therefore NOT captured.
 *
 * Without the --ps3dk-run= marker the hook is inert: no network, no threads, no redirection, exit unchanged.
 * With it: the marker is removed from argv; the child connects back to the host and sends HELLO; writes to fd 1/2
 * are framed into a bounded queue (full queue or lost connection: dropped and counted, never blocking) and still
 * reach the TTY; the process-exit hook (after atexit handlers, static destructors and stdio cleanup) sends EXIT and
 * exitspawns back to the loader; an optional watchdog sends TIMEOUT on its own connection and exitspawns back.
 */
#ifndef PS3DK_RUN_HOOK_H
#define PS3DK_RUN_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 when the hook is active, 0 when inert (no marker), -1 on a malformed marker (left inert).
 * Any failure after the marker parsed (run key, connect, HELLO, watchdog or sender thread) relaunches the loader and
 * does not return; after HELLO the host is told first with a START_FAILED frame (best effort).  -1 is returned then
 * only if the relaunch itself failed.  If the watchdog fired first, its TIMEOUT is the outcome and its relaunch is the
 * only one: -1 is returned (nothing more sent) only if that relaunch failed.  On -1 the child runs on unhooked. */
int ps3dk_run_hook_start(int *argc, const char ***argv);

#ifdef __cplusplus
}
#endif

#endif

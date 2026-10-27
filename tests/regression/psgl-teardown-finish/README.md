# PSGL teardown waits

PSGL teardown must not return while GPU work queued before it is still
pending. Each case queues a GPU wait on test label 201, which a PPU thread
releases after a delay, followed by a back-end write of test label 202. That
work has completed only once label 202 holds its value.

`psgl-teardown-finish`:

- `psglFreeCommandBuffer` returns only after the queued work completed, and
  the buffer is no longer GPU-mapped afterwards.
- If the GPU does not complete within the teardown bound (2 s), the call
  reports `GL_INVALID_OPERATION`, counts one teardown timeout, and keeps the
  buffer mapped and allocated. Freeing it again after the GPU drained
  succeeds and unmaps it.
- If unmapping fails (the test unmaps the buffer itself first), the call
  reports `GL_INVALID_OPERATION` and keeps the buffer registered: freeing it
  again reports the same error, not `GL_INVALID_VALUE`. Once the test maps
  it again, freeing it succeeds and unmaps it.
- `psglExit` waits for queued work even though the application already used
  reference value 1 with its own `cellGcmFinish`. A completion check that
  waits only for the reference register to equal 1 would return at once.

`psgl-teardown-exit-timeout` (built with `TEARDOWN_EXIT_TIMEOUT`): the gate
is released only after the bound, so `psglExit` waits up to the bound, counts
a timeout and stops without completing the teardown. A second `psglExit`
after the GPU drained completes it.

The timeout counter `psgl_teardown_wait_timeouts` is internal to libPSGL and
not public API; the test declares it weak, so a library without it links and
the checks fail at run time.

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

The exit variants make one `psglExit` wait hit its bound, so `psglExit`
stops without shutting down, and check that a second `psglExit` after the
GPU drained shuts down (the internal count `psgl_teardown_shutdowns` rises
by one) and waits on a fresh token (label 255 changes). A retry that did
nothing would fail both.

- `psgl-teardown-exit-timeout` (`TEARDOWN_EXIT_MODE=1`): the gated work is
  queued before `psglExit`, so the first device wait times out and the
  device is left intact.
- `psgl-teardown-exit-flip` (`TEARDOWN_EXIT_MODE=2`): the internal test hook
  `psgl_teardown_test_hook` queues the gated work after the display buffers
  were released, so the last device wait times out. The device is then in
  teardown, and `psglMakeCurrent` refuses to bind it.
- `psgl-teardown-exit-shutdown` (`TEARDOWN_EXIT_MODE=3`): the hook queues
  the gated work at the shutdown wait; the device is already destroyed and
  cannot be bound again.

The bound covers completion polling only: queueing the token reserves FIFO
space first, and that reservation can wait in the context callback.

The timeout counter `psgl_teardown_wait_timeouts`, the shutdown count and the
test hook are internal to libPSGL and not public API; the test declares them
weak, so a library without them links and the checks fail at run time.

# PSGL glFlush and glFinish

`glFlush` must submit queued commands to the GPU, and `glFinish` must not
return until they have completed (OpenGL ES 1.1 section 5.1). The test
writes its own back-end label (index 200) into the command stream after
each draw. The CPU polls that label directly, so no readback call submits or
waits on the test's behalf.

- glFlush: after a draw, a label write and `glFlush`, polling for up to 2 s
  must see the label.
- glFinish: immediately after `glFinish` returns, the label written after
  the preceding draw must be visible. A second draw, label and `glFinish`
  repeat the check with a new value.

A bounded warm-up (at most ten attempts, 200 ms apart, count printed)
through `glReadPixels` absorbs RPCS3 asynchronous shader compilation
skipping a cold draw. Colour-buffer memory is not polled directly, because
RPCS3 does not write rendered surfaces back to guest memory by default.

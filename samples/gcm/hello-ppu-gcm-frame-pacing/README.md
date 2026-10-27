# GCM flip modes and application frame pacing

A moving stripe presents three finite, shader-free phases at the current display
resolution. The top bars identify the phase; the bottom bar shows its progress.

| Phase | Bars / colour | Flip mode | Submission policy |
| --- | --- | --- | --- |
| `vsync` | One / green | VSYNC | Submit after the preceding flip completes |
| `hsync` | Two / orange | HSYNC | Submit after the preceding flip completes |
| `paced-vsync` | Three / blue | VSYNC | Schedule submissions 33,333 microseconds apart |

Each phase submits 120 frames. There is only one outstanding flip, two display
buffers, and a two-second completion timeout. An atomic completion count publishes
timestamps from the flip handler. The PPU requires both the callback and cleared
flip status before reusing the preceding display buffer. XMB exit is polled during
rendering, pacing and flip waits; an in-flight flip is drained within the timeout.
The normal run ends automatically after 360 frames. No controller is required.

`GCM_PACING_PHASE` reports submissions, observed callbacks, application deadlines
missed and minimum/median/maximum callback intervals in microseconds. The median
is the middle of the 119 sorted intervals. In the paced phase, a missed deadline
means a whole scheduled 33,333-us submission period elapsed before submission;
that period is skipped instead of submitting a burst of catch-up frames. The
unpaced phases have no application deadlines and report zero for that counter.

These are application and callback measurements, **not hardware dropped-frame
counters**. Callback scheduling, display refresh, RPCS3 frame limiting and host
load affect the intervals. HSYNC allows tearing by design; this is not a sample
failure. No fixed refresh rate,
relative speedup or absence of tearing is asserted. Screenshots confirm the phase
and moving-stripe scene; the summaries provide the timing observations.
The stripe and bars use scissored clears, relying on RSX clear respecting the
scissor rectangle. A hardware boot also needs visual confirmation of that scene
to establish its appearance; callback summaries alone do not prove the pixels.

Build with the SDK CMake toolchain, then run `hello-ppu-gcm-frame-pacing.fake.self`
in RPCS3, or package/boot it on a PS3. `GCM_PACING_OK phases=3 frames=360` plus
three complete summaries and exit zero is the full-run verdict. `GCM_PACING_QUIT`
is a user-requested early exit, not completion of all phases. Failures exit one.
Retain effective emulator configuration, guest/process exit and fatal log checks.

The sample does not call `gcmSetDefaultFifoSize` or change the initialized FIFO
cursor. Published v0.26.0 still has the post-init rewind bug; hardware validation
must use the private PR54 fix (acb78108) through `PS3DK_GCM_ARCHIVE`, or preserve
this sample's avoidance of the sizing calls, until a release ships that fix.
Mapped FIFO storage remains alive until process teardown. No private shader or
media fixtures are written, and the installed SDK need not be modified.

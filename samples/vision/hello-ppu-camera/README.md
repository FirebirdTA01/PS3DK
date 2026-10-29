# Eye capture and display

This sample opens device 0 at 640x480, 30fps, packed `Y0 U Y1 V`
(`CELL_CAMERA_YUV422`), using function-call read mode. It consumes the
module-owned buffer returned by `cellCameraOpenEx` after each successful
`cellCameraReadEx`, before requesting another frame. Direct-mode buffers,
JPEG and RAW formats are outside this sample's scope.

The PPU converts YUV to RGBA with BT.601 limited-range integer coefficients,
then packs explicit XRGB bytes into the inactive GCM display buffer. Input
and output may be byte-aligned; there are no floating-point loads or typed
unaligned pixel accesses. The camera's actual range/colorimetry is a hardware
validation assumption, not something an emulator frame counter can establish.

Build out of source using the normal PPU CMake toolchain and an installed SDK:

```sh
cmake -S samples/vision/hello-ppu-camera -B "$PS3_BUILD_ROOT/eye-ilp32" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/ps3-ppu-toolchain.cmake"
cmake --build "$PS3_BUILD_ROOT/eye-ilp32" --parallel 2
```

Use a separate directory and `-DCMAKE_C_FLAGS=-mlp64` for LP64. Artifacts keep
the existing names `hello-ppu-camera.self` and `hello-ppu-camera.fake.self`.
Only the inactive display buffer is written; each flip completes before that
buffer can be reused. GCM-mapped host storage remains alive until process exit.

The normal run completes after 120 fresh frames, with a 30-second deadline.
Read stalls and flip stalls have two-second limits. Duplicate frames are
reported and do not count toward completion. The application services the
system exit callback during waits. Return codes from acquisition and cleanup
are reported; resources are released in reverse acquisition order, and the
first failure survives cleanup failures. A camera buffer is never freed by
the sample. A container still referenced after a failed close is retained
until process teardown.

Expected markers:

- `EYE_FORMAT`: requested and returned capture metadata.
- `EYE_STAGE`: each camera acquisition/cleanup result.
- `EYE_SUMMARY`: fresh, duplicate and displayed frames, bytes per frame,
  timestamps, observed rate and cleanup failures. Observed rate includes
  conversion/presentation overhead; it is not a sensor-rate certification.
- `EYE_OK`: all 120 frames displayed and cleanup succeeded.
- `EYE_QUIT`: user exit without cleanup failure; not a completed capture row.
- `EYE_FAIL`: nonzero process result for initialization, capture, presentation,
  metadata, timeout or cleanup failure.

Host tests: `bash tests/host/eye-camera-test.sh`. They cover exact conversion
bytes and malformed-buffer refusal, acquisition failures, reverse cleanup,
error preservation, stale frames, busy/total timeouts and exit handling.

RPCS3 validation uses a private configuration with Camera=Fake and the Eye
camera type, verified in that run's log. Use the default visible window and
the repository's claim/release scripts. RPCS3 `54014a7d` maps Fake to its
`null_camera_handler`, whose image getter always returns zero bytes. This
build therefore exercises the two-second no-frame refusal and cleanup;
it cannot establish capture or image fidelity. Frame-update notifications
do not supply missing pixels. A separately labelled synthetic color control
can validate conversion and display. A Null-camera row must fail without
`EYE_OK` and still clean up.
The pinned emulator sources show the [Fake handler selection](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/rpcs3qt/gui_application.cpp#L755-L763),
[zero-byte image getter](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Io/Null/null_camera_handler.h#L34-L44),
and [ReadEx handler path](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Cell/Modules/cellCamera.cpp#L1350-L1383).
Do not put a camera-positive row in the generic regression manifest under
its default Null-camera configuration.

Capture can be verified only on hardware: a PS3 with an Eye, a control run before and
after, frame/rate and cleanup logs, and the operator's verbatim confirmation
of the displayed image. No hardware result has been established by this code.

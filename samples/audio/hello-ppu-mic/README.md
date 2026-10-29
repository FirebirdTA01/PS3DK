# Eye microphone capture

This C++17 sample reads raw microphone frames and prints a peak, integer RMS
and clipped-sample count for each channel. It builds for ILP32 and LP64.
It does not save raw audio, play it back or use the camera.

The default mode requires a device reported as `CELLMIC_TYPE_EYETOY2` and
requests 48 kHz, four-channel PCM16. The returned format must match exactly;
a lower channel count, another rate or an unsupported encoding is refused.
Actual Eye array capture and physical channel identity are hardware-only
checks and remain **unverified pending a PS3 + Eye session**.

## Build and select an input

Use the normal SDK CMake toolchain, choosing `ps3-ppu-toolchain.cmake` or
`ps3-ppu-toolchain-lp64.cmake`. SELF files go in the build directory:

```sh
cmake -S samples/audio/hello-ppu-mic -B "$PS3_BUILD_ROOT/mic-ilp32" \
  --toolchain "$PS3DEV/share/ps3dev/cmake/ps3-ppu-toolchain.cmake"
cmake --build "$PS3_BUILD_ROOT/mic-ilp32" --parallel 2
```

Configure the console input device before launching. The sample inventories
audio-in devices for diagnostics, then scans attached **mic slots**. Audio-in
inventory numbers and names are not assumed to identify mic slots. It does
not register devices or change the system device mode. Multiple matching Eye
slots require `--device N` (0 through 7); this still checks the Eye type.

Generic USB capture is an explicit alternative:

```text
hello-ppu-mic --generic --device 0 --channels 1
```

Generic mode requires a USB-audio type and accepts a requested channel count
of 1, 2 or 4, again with exact format validation. Logs label this mode
`GENERIC`; it never proves Eye capture. There is no automatic generic fallback.

## Bounds and output

Initialization and discovery have a 10-second deadline. Capture runs for
30 seconds, with a 2-second no-data deadline and at most 10 ms between polls.
Deadlines and the system exit callback are checked after API returns; a stuck
library call requires an external watchdog. Zero PCM samples are valid
silence; zero returned bytes are not evidence of capture.

`MIC_LEVEL` reports 100 ms windows (4800 frames), with a final short window
on successful completion. Values are PCM magnitudes, not calibrated sound
pressure: peak/RMS range from 0 to 32768. Clipping counts either signed
endpoint. Channel indices identify interleaving order, not physical locations.
The parser uses byte loads, explicit endian conversion and integer arithmetic;
partial frames are carried between reads, and a leftover tail at normal
completion fails the run.

`MIC_SUMMARY` records counts, elapsed time, first error, cleanup failures and
retained resources. `MIC_CAPTURE_OK`, `MIC_REFUSED`, `MIC_QUIT` and `MIC_FAIL`
use exit statuses 0, 2, 3 and 1 respectively. A refusal with clean cleanup is
distinct from an execution or cleanup failure.
An initialized session, opened device and started capture are released in
reverse order. A failed stop/close/end prevents unloading potentially live
mic state, is reported as failure and retains the dependent resources.

## Emulator and hardware evidence

RPCS3 at `54014a7d` uses OpenAL host capture and has no fake microphone
generator. Null microphone runs can prove refusal and cleanup only.
Its attachment/type/identity queries are partly placeholders; a reported
attachment is not proof of an Eye. Camera Fake can expose a synthetic
audio-in inventory entry without a working capture stream.
See the pinned public [mic implementation](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Cell/Modules/cellMic.cpp),
[format definitions](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Cell/Modules/cellMic.h)
and [device inventory](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Cell/Modules/cellAvconfExt.cpp).

Measured on that build for both ILP32 and LP64: Null microphone with Camera
Null and Camera Fake refused after discovery (exit 2), and guest exit during
discovery serviced the exit callback (exit 3). All six rows released owned
resources without cleanup errors. No row opened a microphone or read audio;
these are refusal and lifecycle results only.

A positive emulator row needs a deliberately selected host input and proves
only that input's generic capture, parsing and lifecycle. It does not prove
four independent Eye channels, microphone sensitivity or spatial mapping.
The hardware row records actual format, per-channel counts and levels while
the director speaks and during silence, plus the director's observation and
complete cleanup. No hardware result is claimed here yet.

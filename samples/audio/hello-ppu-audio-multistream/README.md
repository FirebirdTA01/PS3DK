# Three-port audio mix

Three independent stereo `cellAudio` ports play C4, E4 and G4 sine tones.
The four phases emphasize C, E and G in turn, then play all three at equal
gain. Each phase submits 375 blocks of 256 frames at 48 kHz (nominally two
seconds). The channels are panned left, centre and right. Green, orange and
blue bars show the three requested port gains; one to four small bars mark
the phase, and the bottom bar shows submission progress.

This uses the actual `cellAudio` port mixer. The SDK's `libmixer` is currently
a placeholder and is not functional; this sample does not call it.

The PPU worker waits on the mixer event queue and feeds all three ports,
while the main thread handles display flips and sysutil callbacks. Both
waits are bounded. An XMB exit request stops the worker and drains the
outstanding flip before cleanup. Normal completion requires all four
phases; early exit prints `AUDIO_MIX_QUIT`, not the full-run success marker.
Only resources acquired by the sample are released.
The main loop has a 30-second cap for the nominal eight-second run. If the
worker does not acknowledge stop within two seconds, the sample exits with
failure and leaves its ports open for process teardown to reclaim; it does
not close resources that a live worker may still access.

## Build

Configure with the installed PS3DK PPU toolchain file and build with CMake.
The LP64 toolchain file is also supported. For example:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake" \
  -DPS3_SELF_OUTPUT_DIRECTORY="$PWD/build"
cmake --build build
```

`PS3DK_GCM_ARCHIVE` optionally selects a private `libgcm_cmd.a`, including
the no-rewind FIFO fix before its release. This sample never calls either
default-FIFO sizing function; it preserves the cursor established by
`cellGcmInit`. It uses scissored clears for the gain display and no shaders.

## Judge the result

The log reports three distinct ports, each phase's gains, submitted blocks,
observed read-index progress and elapsed time. A normal run ends with:

```text
AUDIO_MIX_OK phases=4 streams=3 blocks_per_stream=1500 (submission/lifecycle; audible output checked separately)
```

That marker checks API results, PCM bounds, port progress and cleanup.
It does not prove that every submitted block was played or that sound
reached the speakers. Duplicate read indices are reported rather than
counted as progress. Confirm audible output separately: the low, middle
and high notes should dominate in turn, followed by a balanced chord.
RPCS3 audio dumps can establish the mixed output's frequencies and gain
changes; a real-console listening observation covers the physical output.
Screenshots show gain settings and phase, not sound.

The test does not cover device hotplug, compressed streams, microphone
input, or long-running underrun recovery. The generated PCM has peak
amplitude 0.1 before panning and port gain; this is a short finite example,
not a production audio engine.

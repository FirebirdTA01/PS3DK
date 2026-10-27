# Pad, keyboard and mouse together

Polls port 0 of a pad, a keyboard and a mouse every 8 ms in one loop and
prints a line as soon as a port's connection bit changes, plus a status
block every 60 frames. It stops after 600 frames, on XMB quit, or on pad
START or SELECT. The last line is
`hello-ppu-input-combined: PASS (frames=..., ..., errors=0)` or `FAIL`; the
exit status is 0 or 1.

## What counts as an error

No device on a port, a frame with no new data and a read that fails
because a second info query shows the device was just unplugged are
normal. Any other failed call is an error: init of any surface, registering
or dispatching the sysutil callback, an info query, a read, or a shutdown
call. A setup failure skips the loop but still shuts down whatever was set
up. Pad fields are read only from reports long enough to hold them.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake" \
  -DPS3_SELF_OUTPUT_DIRECTORY="$PWD/build"
cmake --build build
```

`ps3-ppu-toolchain-lp64.cmake` builds the LP64 variant. `MAX_FRAME`
changes the frame count.

## Limitations

A run without devices exercises the calls but not hot-plug; that needs a
USB pad, keyboard or mouse plugged in or removed while the sample runs on
a console. The error paths are covered by
`tests/regression/sample-trio-witness` with faked input calls.

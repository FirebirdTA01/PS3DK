# cellFs asynchronous and streaming reads

Writes a 1 MiB scratch file, reads 64 KiB of it back through
`cellFsAioRead` with a completion callback, submits and cancels a second
AIO read, then streams 256 KiB of the same file through a `cellFsStRead`
ring buffer, checking every block. The last line is
`hello-ppu-cellfs-async-stream: PASS (n of m checks)` or `FAIL`; the exit
status is 0 or 1.

## Scratch file

The file lives in `/dev_hdd0/tmp` under a name built from the process id
and the system time (`ps3tc_aio_stream_<pid>_<time>.bin`, printed at
start). The sample goes on only if `cellFsStat` reports that name does not
exist, creates it with `O_CREAT | O_EXCL`, and removes only the file it
created. Any other stat result stops the run before anything is opened.

## Timeouts

Each AIO request has its own `CellFsAio`, buffer and completion record;
the callback publishes completion with a release store and the main thread
reads the result after an acquire load. The sample waits up to five
seconds (`AIO_TIMEOUT_MS`) for each request. If one has not completed by
then it may still be using the fd and its buffer, so the run stops with
`FAIL` and leaves the fd, the buffers, AIO and the scratch file in place
for process exit to reclaim; the file then stays on disk and its name is in
the output. `cellFsStReadWait` blocks until the data arrives and is not
bounded by the sample.

Whether the cancel wins against the read is printed, not judged. The
cancelled request must still report completion within the timeout.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake" \
  -DPS3_SELF_OUTPUT_DIRECTORY="$PWD/build"
cmake --build build
```

`ps3-ppu-toolchain-lp64.cmake` builds the LP64 variant.

## Limitations

RPCS3's built-in cellFs stops the emulator on `cellFsAioInit` ("cellFsAio
disabled, use LLE"), so the AIO and streaming stages need a real console.
The fault cases (existing scratch name, failed stat, a request that never
completes, a stuck cancel, failed close or AIO finish) are covered by
`tests/regression/sample-trio-witness` with faked cellFs calls.

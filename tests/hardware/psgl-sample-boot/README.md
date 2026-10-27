# PSGL sample hardware boot gate

Boots every `samples/toolchain/hello-psgl*` sample on a PS3 through the
ps3dk-run network loader, so a release can show that each PSGL sample starts,
renders its loop and exits cleanly on real hardware, not only in RPCS3.

This is not a CI test: it needs a console running the ps3dk-run loader
(protocol 2) and a checkout of `spike/ps3dk-run` for the run hook.

## Build

```
build-samples.ps1 -Out <new dir> -HookDir <spike/ps3dk-run checkout> [-Sdk <PS3DK>] [-LibDir <dir>]
```

Each sample is configured with `-DCMAKE_PROJECT_INCLUDE=run-hook-inject.cmake`.
At the end of the sample's own `CMakeLists.txt` that file renames the
sample's `main` to `ps3dk_sample_main` (a compile definition on the sample's
sources only), adds `hook_main.c` as the entry point, and links the hook. The
sample sources are not changed. `hook_main.c` prints
`PSGL_SAMPLE_BOOT hook=<n>`, starts the hook and calls the sample. Without the
loader's argv marker the hook stays inert, so the same SELF boots in RPCS3.

`-RsxCg` selects the rsx-cg-compiler used for shaders and shader libraries.
`-LibDir` puts private ILP32 archives (for example `libPSGL.a`,
`libgcm_cmd.a`) first on the link path, to test a tree before it is
installed. The script writes `selfs.tsv` with each SELF and its sha256.

## Boot

```
boot-samples.ps1 -Selfs <selfs.tsv> -Out <new dir> -Client <ps3dk-run.exe> \
                 -Control <control child SELF> -ConsoleIp <ip> -CallbackHost <ip>
```

Order: the control child, every sample, then the control again, one launch
each, no retries. A row passes when the outcome is `EXITED` with the expected
status (the control's, 7 by default; 0 for a sample), the loader is ready
afterwards, no output was dropped, the SELF hash matches `selfs.tsv`, and
stdout holds the sample's final line from `expect.txt` exactly once with no
`: ... fail` line. Anything else stops the gate at that row. The results are
in `results.tsv`, and the exit status is 0 only when every row passed.

A sample that hangs the GPU can leave the console needing a power cycle; the
gate stops there, and nothing further is launched.

# Mixed Cg / GL program relinking

This regression distinguishes the GL program name remembered by `glUseProgram`
from the vertex and fragment stages actually installed after a direct
`cgGLBindProgram` call. Relinking the remembered GL program must not replace a
direct Cg stage. An explicit `glUseProgram` restores the linked GL executable,
including its current numeric uniform and logical sampler selection.

The witness covers independent vertex and fragment overrides, a direct Cg bind
of the same underlying vertex object (pointer equality alone is insufficient),
successful relinking of an active GL executable, and a failed relink followed by
a successful repair. An active program with no samplers changes from a constant
yellow fragment executable to cyan on relink, without another `glUseProgram`.
Read-only context observations check the installed stages
and routing flags; binding, uniforms and drawing use public APIs. Shader
containers are embedded and loaded through `cgCreateProgram(CG_OBJECT)`.

Every pixel comparison uses a bounded ten-attempt warm-up for RPCS3 asynchronous
shader compilation. The runner must retain the effective configuration and
attempt counts. A separate Legacy shader-mode run must pass each comparison on
its first attempt. Acceptance also requires the exact final `PSGL_RELINK_OK`
marker, exit zero, process exit, and no fatal log lines; exit zero alone is not
the verdict.

Both ILP32 and LP64 targets are built. `PSGL_ARCHIVE_ilp32/lp64` and
`GCM_ARCHIVE_ilp32/lp64` allow private archives without modifying the installed
SDK. Hardware runs against the published v0.26.0 SDK require the private GCM
no-rewind fix from acb78108; the installed release's FIFO sizing can rewind a
live command buffer.

This test does not change deferred program deletion (t_9969a59e), shared-shader
numeric uniform ownership, or the remembered GL name after a direct Cg bind.
Those lifetime and per-program storage issues remain separate work.

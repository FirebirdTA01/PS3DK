# Public Cg program creation and parameter classes

The witness creates programs through the public `cgCreateProgram` from
precompiled containers, both the raw `.vpo`/`.fpo` container and the compact
CGB container, with `CG_BINARY` and `CG_OBJECT`. A `CG_PROFILE_UNKNOWN`
profile takes the profile from the container. The container is copied, so
the caller's buffer may be released after creation. The raw pair, and a
fragment program whose source buffer was released, draw an exact color
after bounded async shader warm-up.

`cgCreateProgram` takes no length: the caller must pass a complete,
readable container that matches its own header. The runtime reads the size
from that header (raw `totalSize`, or the compact container's own block
offsets) and copies that many bytes, so it cannot detect a short buffer;
the 16 MiB cap only refuses an absurd declared size and is not bounds or
truncation protection.

Refusals return NULL, report one exact `cgGetError` value and add no program
to the context: invalid context, NULL program, `CG_SOURCE` (with a listing
naming the offline compiler), an unknown program type, bytes that are not a
container, a wrong format revision, a declared `totalSize` too small for the
container's own offsets (the backing buffer stays full size) or absurdly
large, a container of the other stage, and an unsupported profile.

`cgCreateProgramFromFile` accepts `CG_OBJECT`: a program is created from a
real container file with the profile taken from the container, and still
draws after the file is removed. A file truncated to half its container is
refused with `CG_PROGRAM_LOAD_ERROR`; here the file length is known.

The fixture paths are preserved by `fixture_files.h`: snapshots of both
paths are taken before any write, only a confirmed `ENOENT` counts as
absent (any other stat error fails closed), and a path is restored only
from a completed snapshot; a failed restoration is reported. If either
snapshot fails, neither path is touched and the dependent checks fail.
`fixture_files_check.h` checks those helpers against an in-memory file
system with injected faults (stat error, snapshot read error, snapshot
close error, allocation failure, failed second snapshot): the original
bytes stay unchanged and no write or remove happens. It also checks normal
restoration of a present and of an absent path, and that an absent-path
restore with a stat error fails closed. `cgGetLastErrorString` reports the pending error and
its message, then clears it.

Parameter classes come from the raw container's per-parameter types:
scalar, vector and every sampler kind, plus `cgGetTypeClass` over scalar,
vector, matrix, sampler, struct, array and object types.

The compact container carries no per-parameter Cg types, so its parameters
are not class-checked here. Creating a compact fragment program succeeds,
but rendering with it currently fails (it draws nothing): a known defect,
tracked separately. Compact runtime support is not claimed; each stage
combination is recorded as an observation line (`PSGL_CG_CREATE_OBSERVE`)
with the parameter lists, without a check. Both ABIs run against private archives. No
hardware acceptance is claimed.

# Public Cg program creation and parameter classes

The witness creates programs through the public `cgCreateProgram` from
precompiled containers, both the raw `.vpo`/`.fpo` container and the compact
CGB container, with `CG_BINARY` and `CG_OBJECT`. A `CG_PROFILE_UNKNOWN`
profile takes the profile from the container. The container is copied, so
the caller's buffer may be released after creation. The raw pair, and a
fragment program whose source buffer was released, draw an exact color
after bounded async shader warm-up.

Refusals return NULL, report one exact `cgGetError` value and add no program
to the context: invalid context, NULL program, `CG_SOURCE` (with a listing
naming the offline compiler), an unknown program type, bytes that are not a
container, a wrong format revision, a truncated or oversized size field, a
container of the other stage, and an unsupported profile.
`cgCreateProgramFromFile` accepts `CG_OBJECT` as well.

Parameter classes come from the raw container's per-parameter types:
scalar, vector and every sampler kind, plus `cgGetTypeClass` over scalar,
vector, matrix, sampler, struct, array and object types.

The compact container carries no per-parameter Cg types, so its parameters
are not class-checked here. Drawing with compact containers is outside this
row's claim: each stage combination is recorded as an observation line
(`PSGL_CG_CREATE_OBSERVE`) with the parameter lists, without a check. Both ABIs run against private archives. No
hardware acceptance is claimed.

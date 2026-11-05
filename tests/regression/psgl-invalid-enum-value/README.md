# GL ES 2.0 draw and enable error rules (t_3093d190)

Pins the error rules for the PSGL draw and enable entry points so a
regression that silently drops a GL error is caught. All rows run in one
made-current context and check the first error (or its absence) returned
by `glGetError`. No scene, program, buffer, or pixel readback is needed:
for `glDrawArrays` / `glDrawElements` the error checks fire before the
buffer lookup and draw dispatch, and `count == 0` returns before that too.

Rules asserted (see `sdk/libPSGL/src/psgl_context.c`):

- `glDrawArrays` / `glDrawElements`:
  - an unsupported primitive mode -> `GL_INVALID_ENUM` (checked first, so it
    is the error reported even if the other arguments are also bad);
  - `glDrawArrays` with `first < 0`, or either with `count < 0` ->
    `GL_INVALID_VALUE`;
  - `count == 0` -> a legal no-op: the call returns with no draw and no
    error (it is not an error in ES 2.0).
- `glDrawElements`:
  - an index `type` that is not one of `GL_UNSIGNED_BYTE`,
    `GL_UNSIGNED_SHORT`, `GL_UNSIGNED_INT` -> `GL_INVALID_ENUM`;
  - `GL_UNSIGNED_BYTE` is a legal ES 2.0 index type, so it is NOT an
    error and draws nothing today. The command set has no 8-bit index
    type, so byte-indexed draws are a no-op until a separate widen-to-16
    path lands; that follow-up is tracked on its own card and is out of
    scope here, which is why this row asserts `GL_NO_ERROR` (legal no-op),
    not `GL_INVALID_ENUM`.
- `glEnable` / `glDisable` with a capability that is neither a known cap
  nor in the `GL_LIGHT0..` range -> `GL_INVALID_ENUM`; a recognized cap
  keeps working and an unknown cap must not change an already-set cap
  (observed with `GL_SCISSOR_TEST`, the one public cap query this build
  supports).

`psgl_gl_error` records the first error on the context, so each row runs
after clearing any prior error and sees only the error its call raised.

# Attribute normalization and disabled arrays

The normalization row uses a vertex shader that divides color by 255.
An UNSIGNED_BYTE input of 255 must produce framebuffer byte 1 when
normalized, and byte 255 when not normalized. Both VBO and client arrays
are checked. The disable row checks that disabling a legacy or direct-Cg
color array restores the current color rather than reusing stale data.

The direct-Cg fixture uses `psgl_cg_create_program_from_memory` to load
embedded binaries. Public `cgCreateProgram` currently returns NULL even
for binary input, so it cannot set up this fixture. The internal loader
is globally defined in both ABI archives in the published v0.26.0 SDK,
but is not a public API contract. Binding and array operations under test
use public Cg functions. Installed-archive link checks are separate from
private-archive runtime checks; v0.26.0 also lacks the working ReadPixels
implementation needed for these pixel verdicts.

Pixel checks allow bounded asynchronous shader warm-up and require exact
final RGBA bytes. These rows do not establish hardware acceptance or
general GL conformance.

# Fragment sampler2D arrays

This fixture compiles genuine Cg array declarations and exercises GL routing
on both PPU ABIs. It checks reflected element identity independently of query
order and physical texture slot order. Red/blue come from the first sampled
element and green from the second; eight distinct textures make swapped
elements visible. Each pixel check clears the target and permits a bounded
shader warm-up before requiring exact RGBA bytes.

Coverage includes default zero, base-name/[0] aliasing, element queries,
all logical units 0-7, partial/full/excess updates, unused middle/trailing
elements, scalar glUniform1i updates, shared shaders, program switching,
successful/failed relink, and direct Cg binding. A reserved scalar physical
slot separates the array's two hardware slots. Duplicate physical slots,
overlong names, descriptor exhaustion and out-of-range resources refuse link.

glUniform1iv supports reflected fragment sampler2D selections only. Negative
count is INVALID_VALUE. Excess array input is ignored after the remaining
declared extent; count greater than one on a scalar is INVALID_OPERATION.
All effective input values are validated before any state change. Units
outside 0-7 and a null pointer with positive effective count are refused with
INVALID_VALUE as implementation guards. Zero count makes no input reads or
state changes; location -1 is ignored for otherwise valid calls. Numeric and
cube vector updates remain unsupported. Existing numeric glUniform1i behavior
is separate. A getter at an element location returns that element alone.

The weak glUniform1iv reference lets unchanged libraries execute the RED:
absence explicitly fails the entry-point and setter assertions. No replacement
implementation is provided. Other GL calls extract gl_shader.o from the same
archive. RED and GREEN must use identical fixture and shader bytes.

This is an emulator regression, not a hardware or ES2 conformance claim.
Global GL object lifetime and stale locations across relinks are outside its
scope. Cube textures and mixed sampler-type validation are separate work.

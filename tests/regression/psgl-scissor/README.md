# Scissor state and clipping

This regression checks signed scissor state and the integer/float/boolean
queries, initial surface dimensions, negative-size rejection without mutation,
disabled-state persistence, and lower-left clipping on the default framebuffer
and an 8x6 reference FBO. Clears and draws are checked separately. Oversized,
empty and out-of-target boxes include 32-bit endpoint-overflow cases.

An immutable white fragment binary is warmed up with bounded retries before
each target's tested sequence. The tests themselves require exact pixels,
without retries. No changing fragment uniform or shared-ucode ordering is
involved. All readback goes through the separately tested RGBA byte path.
The installed companion SDK has the recorded GCM Phase B overlay; private
PSGL archives are used without replacing the SDK. No hardware, MSAA or
general ES conformance claim is made. Context-rebind state resets, viewport
validation and other glClear masking behavior are separate audit gaps.

Contract: OpenGL ES 2.0.25 sections 4.1.2 and 4.2.3:
https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf

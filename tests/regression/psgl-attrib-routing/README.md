# Logical GL attribute routing

This row binds POSITION to logical location 7 and COLOR to 5 while the
binary consumes physical inputs 0 and 3. It checks pending bindings,
relinking the current program without another UseProgram call, switching
programs, VBO/client arrays, disabled current values and all eight logical
locations. Pixel checks use bounded asynchronous-shader warm-up and exact
final colors. A synchronous-shader run must need one draw per check.
Further cases cover nonzero-first client arrays, failed relink/use retaining
the installed executable, inactive bindings, invalid pointer state updates,
and switches between logical GL, direct Cg and legacy arrays. The Cg fixture
uses the internal binary-memory loader because the public Cg memory loader
is still unimplemented; the array/bind calls themselves use the Cg API.
The loader was globally defined in both ABI archives of the tested install,
though it is not a public API contract. That install included the GCM Phase B
overlay; it was not a pristine published v0.26.0 extraction. Installed-archive
link checks are kept separate from private-archive runtime validation.
Both ABIs must finish with `PSGL_ATTR_OK checks=80 failures=0`.

Bindings become visible only after a successful link. Unbound active
inputs receive the lowest free locations in ascending physical-input
order, independent of the binary metadata order. Unused names return -1;
the previous name-based position/color defaults no longer apply.

This increment does not establish ES 2.0 or desktop GL 2.1 conformance.
The current RSX-facing pointer path accepts FLOAT, UNSIGNED_BYTE and
UNSIGNED_SHORT and refuses stride above 255; BYTE, SHORT and FIXED remain
unsupported despite being valid ES 2.0 types. These are implementation
limitations, not ES 2.0 restrictions. Normalization has a separate repair
and regression. DrawElements still lacks client-array uploading.

The compatibility path aliases generic location zero to the legacy vertex
array when the pointer/enable call is made with no Cg/GL vertex program
bound. GL shader-object arrays stay separate from legacy arrays; switching
back to fixed function does not retroactively alias state set while a
program was bound. This falls short of the unconditional desktop GL 2.1
alias. Direct Cg input indices remain separate from logical GL locations.

For correctness, each dirty routed-array emission disables all 16 physical
array descriptors before installing the active mapping. This adds 16
commands per dirty emission; no performance improvement is claimed.
Pending name bindings currently retain the pre-existing eight-name storage
limit and 63-character names. These limits also require further audit.

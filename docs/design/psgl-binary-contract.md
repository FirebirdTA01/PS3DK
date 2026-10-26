# PSGL precompiled shader API contract

The ES 2.0 direction in `gl-standards-study.md` starts with precompiled shaders.
This increment covers shader binary loading, compiler availability queries and
their errors. It does not establish ES 2.0 conformance or change the advertised
GL version. Error checking in the remaining fixed-function API is a separate
audit; the error flag is not evidence that every entry point validates arguments.
Unaudited integer state queries retain their legacy zero fallback rather than
being misclassified as invalid enums. They do not yet report complete GL state.

The public specification is [OpenGL ES 2.0.25](https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf),
sections 2.10.1, 2.10.2 and 6.1.8. With `GL_SHADER_COMPILER` false, applications
load each shader using `glShaderBinary`, then attach and link it. They must not
call `glShaderSource` or `glCompileShader` after loading a binary. Check
`glGetError` after upload and `GL_LINK_STATUS` after linking.

`GL_SHADER_BINARY_FORMAT_CGB_PS3DK` represents one CGB vertex or fragment
container and one shader object per upload. Its stage must match the object.
It does not represent a combined vertex/fragment executable. Negative sizes,
unknown shader names, invalid containers and unsupported multi-object containers
are rejected; repeated shader stages are an invalid operation. Invalid format
tokens are invalid enums. Empty uploads do not create executable shader state.

The compiler-less restrictions include shader source/precision retrieval and
compiler release. Shader `COMPILE_STATUS`, `INFO_LOG_LENGTH` and
`SHADER_SOURCE_LENGTH` queries are unavailable. In particular, section 6.1.8,
printed page 131, places **both** `glGetShaderInfoLog` and
`glGetProgramInfoLog` under the no-compiler restriction. The preceding
`glGetProgramiv` definition does not restrict `GL_INFO_LOG_LENGTH`; that query
remains available. Applications should not request a program log after a failed
link in this mode. Error queries consume the current context's first pending
error; errors in another context do not overwrite it. Failed output queries
leave their destination unchanged.

The single-stage CGB extension rejects two different shader stages in one upload
with `GL_INVALID_VALUE`; two handles for the same stage use the ES2-mandated
`GL_INVALID_OPERATION`. Previously linked programs retain their Cg executable
when a shader is reloaded. The existing process-global Cg context retains those
allocations, so repeated successful uploads grow memory by the container and its
parameter metadata per upload for the process lifetime. This increment does not
introduce reference-counted GL object reclamation; applications should load a finite set
of binaries rather than reload them every frame.

The format queries report the CGB shader format and the four implemented DXT
texture enums (RGB/RGBA DXT1, RGBA DXT3 and RGBA DXT5). Query results are available
through integer, boolean and float query entry points.

`tests/regression/psgl-binary-contract` exercises this contract on both PPU ABIs.
The existing `hello-psgl-glsl-object` sample exercises the binary-loading path
while drawing; its host GLSL helper is a fixed-template selector, not a GLSL
language validator. Neither test establishes full shader-language conformance.

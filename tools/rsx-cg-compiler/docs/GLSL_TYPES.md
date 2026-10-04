# Optional GLSL type names

`--extension=glsl-types` enables these additional spellings:

| Spelling | Existing Cg type |
|---|---|
| `vec2`, `vec3`, `vec4` | `float2`, `float3`, `float4` |
| `ivec2`, `ivec3`, `ivec4` | `int2`, `int3`, `int4` |
| `bvec2`, `bvec3`, `bvec4` | `bool2`, `bool3`, `bool4` |
| `dvec2`, `dvec3`, `dvec4` | Cg `double2`, `double3`, `double4`, which this compiler represents as float |
| `mat2`, `mat3`, `mat4` | `float2x2`, `float3x3`, `float4x4` |
| `matRxC`, with R and C from 2 through 4 | Cg `floatRxC` |

These are type-name aliases within a Cg program. Matrix indexing remains Cg row
indexing: `mat2x3` has two rows of three components. A scalar matrix constructor
fills every component, as the corresponding Cg constructor does. This extension
does not implement GLSL column-major constructor ordering, diagonal scalar
construction, or 64-bit floating-point arithmetic.

Source functions, variables, parameters, structs, and typedefs retain precedence
over the optional names in their scopes. The names are not reserved keywords.
Disabled declarations, casts and reachable unresolved constructor calls receive
a diagnostic naming the enabling flag. Disabled mode retains existing
unknown-name reachability checks in unused helpers. Existing conversion
and backend refusals remain in force, including runtime integer/bool-to-float
conversion in vertex programs. GLSL function names are controlled separately by
`--extension=glsl-functions`.

Acceptance under this flag is an intentional extension of the Cg source surface;
it is not counted as unflagged reference-compiler acceptance. Tests compare the
enabled spellings with their explicit Cg counterparts and check output values.

#!/usr/bin/env python3
import sys
from pathlib import Path


def emit_vertex(source: str) -> str:
    required = [
        "attribute vec3 in_position",
        "uniform mat4 modelViewProj",
        "gl_Position",
    ]
    if not all(token in source for token in required):
        raise SystemExit("unsupported vertex GLSL subset")
    return """void main(float3 in_position : POSITION,
          uniform float4x4 modelViewProj,
          out float4 out_position : POSITION)
{
    out_position = mul(modelViewProj, float4(in_position, 1.0));
}
"""


def emit_fragment(source: str) -> str:
    required = [
        "uniform vec4 solidColor",
        "gl_FragColor",
    ]
    if not all(token in source for token in required):
        raise SystemExit("unsupported fragment GLSL subset")
    return """void main(uniform float4 solidColor,
          out float4 out_color : COLOR)
{
    out_color = solidColor;
}
"""


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in ("vertex", "fragment"):
        print("usage: glsl_subset_to_cg.py <vertex|fragment> <input.glsl> <output.cg>",
              file=sys.stderr)
        return 2

    shader_kind = sys.argv[1]
    source = Path(sys.argv[2]).read_text(encoding="utf-8")
    output = emit_vertex(source) if shader_kind == "vertex" else emit_fragment(source)
    Path(sys.argv[3]).write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Compare composed aliases with independently expanded lane expressions.

Overwriting, rather than composing, the source swizzle breaks these pairs.
The DIVR test separately pins the original witness's decoded input lanes;
these pairs extend that property across operators and both profiles.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


CASES = [
    ("move", "float4 b = a.wzyx;", "b.yzxw", "a.zywx"),
    ("add", "float4 b = a.wzyx;", "b.yzxw + u", "a.zywx + u"),
    ("mul", "float4 b = a.wzyx;", "b.yzxw * u", "a.zywx * u"),
    ("dot", "float4 b = a.wzyx;", "dot(b.yzxw, u) * u", "dot(a.zywx, u) * u"),
    ("chain3", "float4 b = a.wzyx; float4 c = b.yzxw;",
     "c.zwxy", "a.wxzy"),
    ("uniform", "float4 b = u.wzyx;", "b.yzxw + a", "u.zywx + a"),
    ("computed", "float4 v = a + u; float4 b = v.wzyx;",
     "b.yzxw", "v.zywx"),
    ("scalar_x", "float4 b = a.wzyx;", "b.x * u", "a.w * u"),
    ("scalar_w", "float4 b = a.wzyx;", "b.w * u", "a.x * u"),
    ("extract", "float4 b = a.wzyx;", "b[2] * u", "a.y * u"),
    ("index_direct", "", "a[2] * u", "a.z * u"),
    ("narrow", "float3 b = a.wzy;", "b.yxx * u.xyz, 1.0", "a.zww * u.xyz, 1.0"),
]


def source(profile, setup, expr):
    outputs = "out float4 o : COLOR"
    passthrough = ""
    if profile == "vp":
        outputs = "out float4 position : POSITION, out float4 o : TEXCOORD1"
        passthrough = "position = p;"
    return ("void main(float4 p : POSITION, float4 a : TEXCOORD0, "
            "uniform float4 u, " + outputs + ") {\n" + passthrough + "\n" +
            setup + "\n o = float4(" + expr + ");\n}\n")


def main():
    compiler = sys.argv[1]
    failures = []
    count = 0
    with tempfile.TemporaryDirectory(prefix="ps3dk-local-swizzle-") as work:
        root = Path(work)
        for profile in ("fp", "vp"):
            for name, setup, composed, direct in CASES:
                binaries = []
                for tag, expression in (("composed", composed), ("direct", direct)):
                    path = root / (profile + "_" + name + "_" + tag + ".cg")
                    # The direct computed case still needs v, but no alias.
                    declarations = setup if tag == "composed" else (
                        "float4 v = a + u;" if name == "computed" else "")
                    path.write_text(source(profile, declarations, expression))
                    output = path.with_suffix(".bin")
                    result = subprocess.run(
                        [compiler, "-p", "sce_" + profile + "_rsx", "--emit-container", str(output), str(path)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=int(os.environ.get("PS3TC_SWIZZLE_TIMEOUT", "15")),
                    )
                    if result.returncode or not output.exists():
                        raise SystemExit("FAIL: %s did not compile:\n%s" %
                                         (path.name, result.stdout.decode(errors="replace")))
                    binaries.append(output.read_bytes())
                count += 1
                if binaries[0] != binaries[1]:
                    failures.append(profile + "/" + name)
    if failures:
        raise SystemExit("FAIL: composed/direct containers differ: " + ", ".join(failures))
    print("local-swizzle-test: PASS %d composed/direct pairs" % count)


if __name__ == "__main__":
    main()

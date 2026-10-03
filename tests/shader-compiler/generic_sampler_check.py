"""Generic Cg samplers retain distinct type identity and measured 2D bindings.

The native oracle accepts separate sampler/sampler2D overloads and conversions
between those types. Supported generic 2D fetches produce the corresponding
sampler2D container, including CGtype1066 reflection. Other generic dimensions
are ordinary valid Cg but remain explicitly unsupported in this bounded slice.
All shader sources below are independently authored; no SDK is required.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from entry_sampler_check import check, compile_one, require
from fp_sources import instructions, ucode_words


# name, stage, generic source, typed control, sampler records, fetch units,
# additional interface records. G is a whole type token in authored templates.
CASES = []


def add(name, template, samplers, units, stage="fp", resources=None):
    CASES.append((name, stage, template.replace("G", "sampler"),
                  template.replace("G", "sampler2D"), samplers, units, resources or {}))


# Five reduced patterns cover explicit entry samplers, globals, multiple units,
# repeated horizontal/vertical fetches, and a one-sampler colour transformation.
add("entry_two_units",
    "float4 main(float2 uv:TEXCOORD0,uniform G a:register(s0),uniform G b:register(s1)):COLOR"
    "{return tex2D(a,uv)*0.25+tex2D(b,uv)*0.75;}",
    {"a": (1066, 0, 1), "b": (1066, 1, 1)}, [0, 1])
add("global_two_units",
    "G a:register(s1); G b:register(s3); float4 main(float2 uv:TEXCOORD0):COLOR"
    "{return tex2D(a,uv)+tex2D(b,uv)*0.5;}",
    {"a": (1066, 1, 1), "b": (1066, 3, 1)}, [1, 3])
for axis, offset in (("horizontal", "float2(0.125,0)"), ("vertical", "float2(0,0.25)")):
    add(axis + "_samples",
        "G s:register(s2); float4 main(float2 uv:TEXCOORD0):COLOR"
        "{return tex2D(s,uv)*0.5+tex2D(s,uv+" + offset + ")*0.25+tex2D(s,uv-" + offset + ")*0.25;}",
        {"s": (1066, 2, 1)}, [2, 2, 2])
add("entry_colour_transform",
    "float4 main(float2 uv:TEXCOORD0,uniform G s:register(s2)):COLOR"
    "{return saturate(tex2D(s,uv)-float4(0.1,0.2,0.3,0.4));}",
    {"s": (1066, 2, 1)}, [2])

for label, qualifier in (("none", ""), ("in", "in "), ("uniform", "uniform ")):
    add("entry_" + label,
        "void main(" + qualifier + "G s:register(s2),float2 uv,out float4 colour)"
        "{colour=tex2D(s,uv);}", {"s": (1066, 2, 1)}, [2],
        resources={"uv": (3220, 4101, 1), "colour": (2757, 4101, 1)})
add("unused_entry",
    "float4 main(uniform G s:register(s2),float4 uv:TEXCOORD0):COLOR{return uv;}",
    {"s": (1066, 2, 0)}, [])

for formal, actual, name in (("G", "G", "generic_to_generic"),
                              ("sampler2D", "G", "generic_to_2D"),
                              ("G", "sampler2D", "2D_to_generic")):
    add("helper_" + name,
        "float4 fetch(" + formal + " t,float2 uv){return tex2D(t,uv);}\n" +
        actual + " s:register(s2); float4 main(float2 uv:TEXCOORD0):COLOR{return fetch(s,uv);}",
        {"s": (1066, 2, 1)}, [2])

for name, fetch in (("lod", "tex2Dlod"), ("bias", "tex2Dbias"), ("projected", "tex2Dproj")):
    add("fragment_" + name,
        "G s:register(s2); float4 main(float4 uv:TEXCOORD0):COLOR{return " + fetch + "(s,uv.wzyx);}",
        {"s": (1066, 2, 1)}, [2])
for name, expression in (("plain", "tex2D(s,p.wz)"), ("lod", "tex2Dlod(s,p.wzyx)")):
    add("vertex_" + name,
        "G s:register(s2); float4 main(float4 p:POSITION):POSITION{return " + expression + ";}",
        {"s": (1066, 2, 1)}, [2], stage="vp")

# Distinct overloads must remain distinct; exact matches beat the measured
# generic<->2D conversions in both directions. An alias implementation fails.
for name, actual, expected in (("generic", "sampler", "float4(1,2,3,4)"),
                                ("typed", "sampler2D", "float4(5,6,7,8)")):
    declarations = ("float4 fetch(sampler s){return float4(1,2,3,4);}\n"
                    "float4 fetch(sampler2D s){return float4(5,6,7,8);}\n")
    source = declarations + actual + " s:register(s2); float4 main(float4 p:TEXCOORD0):COLOR{return fetch(s);}"
    twin = "sampler2D s:register(s2); float4 main(float4 p:TEXCOORD0):COLOR{return " + expected + ";}"
    CASES.append(("distinct_overload_" + name, "fp", source, twin, {"s": (1066, 2, 0)}, [], {}))


REFUSALS = []
for dimension, coordinate in (("1D", "p.x"), ("3D", "p.xyz"), ("CUBE", "p.xyz"), ("RECT", "p.xy")):
    REFUSALS.append(("generic_" + dimension, "fp",
        "sampler s:register(s2); float4 main(float4 p:TEXCOORD0):COLOR{return tex" + dimension + "(s," + coordinate + ");}",
        "generic-sampler-dimension"))
REFUSALS += [
    ("generic_mixed_dimensions", "fp",
     "sampler s:register(s2); float4 main(float4 p:TEXCOORD0):COLOR{return tex2D(s,p.xy)+texCUBE(s,p.xyz);}",
     "generic-sampler-dimension"),
    ("generic_cube_vertex", "vp",
     "sampler s:register(s2); float4 main(float4 p:POSITION):POSITION{return texCUBE(s,p.xyz);}",
     "generic-sampler-dimension"),
    ("generic_to_cube_helper", "fp",
     "float4 fetch(samplerCUBE t,float3 p){return texCUBE(t,p);}\n"
     "sampler s:register(s2); float4 main(float3 p:TEXCOORD0):COLOR{return fetch(s,p);}",
     "generic-sampler-dimension"),
    ("cube_to_generic_helper", "fp",
     "float4 fetch(sampler t,float2 p){return tex2D(t,p);}\n"
     "samplerCUBE s:register(s2); float4 main(float2 p:TEXCOORD0):COLOR{return fetch(s,p);}",
     "no matching function"),
    ("typed_dimension_mismatch", "fp",
     "sampler2D s:register(s2); float4 main(float3 p:TEXCOORD0):COLOR{return texCUBE(s,p);}", "texCUBE"),
    ("unknown_type", "fp",
     "samplerUnknown s; float4 main(float2 p:TEXCOORD0):COLOR{return tex2D(s,p);}", "unknown type"),
    ("generic_out", "fp",
     "float4 main(float2 p:TEXCOORD0,out sampler s):COLOR{return tex2D(s,p);}", "sampler operand"),
    ("generic_inout", "fp",
     "float4 main(float2 p:TEXCOORD0,inout sampler s):COLOR{return tex2D(s,p);}", "sampler operand"),
]


def run_checks(compiler, work):
    failures = []
    for name, stage, text, twin, samplers, units, resources in CASES:
        blobs = {}
        for label, source in (("typed_control", twin), ("generic", text)):
            try:
                rc, blob, log = compile_one(compiler, work, name + "_" + label, stage, source)
                require(rc == 0 and bool(blob), "compile rc=%d: %s" % (rc, log.strip()[-700:]))
                check(blob, stage, samplers, units, resources)
                if stage == "fp":
                    for words, _ in instructions(ucode_words(blob)):
                        if (words[0] >> 24) & 63 in (0x17, 0x18, 0x2F, 0x31):
                            require(words[3] >> 31 == 0, "2D fetch unexpectedly disables perspective correction")
                blobs[label] = blob
                print("checked " + name + "_" + label)
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                failures.append(name + "_" + label + ": " + str(error))
        if len(blobs) == 2 and blobs["typed_control"] != blobs["generic"]:
            failures.append(name + ": generic spelling changed complete measured 2D container")
    for name, stage, text, diagnostic in REFUSALS:
        try:
            rc, blob, log = compile_one(compiler, work, name, stage, text)
            require(rc == 1 and not blob and diagnostic in log,
                    "expected named refusal, got rc=%d bytes=%d: %s" % (rc, len(blob), log.strip()[-700:]))
            print("checked refusal " + name)
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append(name + ": " + str(error))
    for failure in failures:
        print("FAIL: " + failure)
    print("generic-sampler: " + ("FAIL (%d)" % len(failures) if failures else "PASS"))
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run_checks(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix="generic-sampler-") as directory:
        return run_checks(args.compiler, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())

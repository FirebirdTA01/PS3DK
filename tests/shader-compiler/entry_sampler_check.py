"""Selected-entry typed samplers have uniform domain, regardless of input spelling.

Measured on the native reference: absent/in sampler qualifiers produce the exact
uniform container across 1D/2D/3D/CUBE fragment samplers and supported vertex
cases. A const sampler2D entry also has uniform reflection. Helpers retain their
ordinary parameter binding; output and inout samplers remain refused.

Compare complete compiler-produced twins and independently check reflection and
actual fetch units. This test needs no reference compiler or private SDK.
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

from fp_sources import instructions, ucode_words


# name, stage, source template, sampler name -> (Cg type, unit, referenced),
# expected units on actual texture instructions, additional parameter resources.
CASES = []
for kind, type_id, expression in (
        ("1D", 1065, "tex1D(s,p.x)"), ("2D", 1066, "tex2D(s,p.xy)"),
        ("3D", 1067, "tex3D(s,p.xyz)"), ("CUBE", 1069, "texCUBE(s,p.xyz)")):
    CASES.append(("sample_" + kind, "fp",
                  "float4 main(float4 p:TEXCOORD0,Qsampler" + kind +
                  " s):COLOR{return " + expression + ";}",
                  {"s": (type_id, 0, 1)}, [0], {}))
    CASES.append(("unused_" + kind, "vp",
                  "float4 main(float4 p:POSITION,Qsampler" + kind +
                  " s:TEXUNIT1):POSITION{return p;}",
                  {"s": (type_id, 1, 0)}, [], {}))

CASES += [
    ("vertex_lod", "vp",
     "float4 main(float4 p:POSITION,Qsampler2D s):POSITION{return tex2Dlod(s,float4(p.xy,0,0));}",
     {"s": (1066, 0, 1)}, [0], {}),
    ("implicit_varying", "fp",
     "void main(Qsampler2D s,float2 uv,out float4 colour){colour=tex2D(s,uv);}",
     {"s": (1066, 0, 1)}, [0], {"uv": (3220, 4101, 1), "colour": (2757, 4101, 1)}),
    ("explicit_registers", "fp",
     "float4 main(float4 p:TEXCOORD0,Qsampler2D a:register(s3),Qsampler2D b:register(s1)):COLOR{return tex2D(a,p.xy)*0.25+tex2D(b,p.zw)*0.75;}",
     {"a": (1066, 3, 1), "b": (1066, 1, 1)}, [3, 1], {}),
    ("unused_inputs", "fp",
     "void main(Qsampler2D unused,Qsampler2D live,float2 dead,float2 uv,out float4 colour){colour=tex2D(live,uv);}",
     {"unused": (1066, None, 0), "live": (1066, 0, 1)}, [0],
     {"dead": (3256, 4101, 0), "uv": (3220, 4101, 1), "colour": (2757, 4101, 1)}),
    ("helper_argument", "fp",
     "float4 fetch(in sampler2D s,float2 uv){return tex2D(s,uv);}\n"
     "float4 main(float2 uv:TEXCOORD0,Qsampler2D s:TEXUNIT3):COLOR{return fetch(s,uv);}",
     {"s": (1066, 3, 1)}, [3], {}),
    ("fragment_lod", "fp",
     "float4 main(float4 uv:TEXCOORD0,Qsampler2D s:TEXUNIT2):COLOR{return tex2Dlod(s,uv);}",
     {"s": (1066, 2, 1)}, [2], {}),
    ("vertex_explicit_units", "vp",
     "float4 main(float4 p:POSITION,Qsampler2D a:TEXUNIT2,Qsampler2D b:TEXUNIT0):POSITION{return p+tex2Dlod(a,float4(p.xy,0,0))*0.25+tex2Dlod(b,float4(p.zw,0,0))*0.75;}",
     {"a": (1066, 2, 1), "b": (1066, 0, 1)}, [2, 0], {}),
    ("const_input", "fp",
     "float4 main(float2 uv:TEXCOORD0,Qsampler2D s):COLOR{return tex2D(s,uv);}",
     {"s": (1066, 0, 1)}, [0], {}),
]

REFUSALS = [
    ("out_sampler", "fp",
     "float4 main(float2 uv:TEXCOORD0,out sampler2D s):COLOR{return tex2D(s,uv);}",
     "sampler operand"),
    ("inout_sampler", "fp",
     "float4 main(float2 uv:TEXCOORD0,inout sampler2D s):COLOR{return tex2D(s,uv);}",
     "sampler operand"),
    ("inout_sampler_vp", "vp",
     "float4 main(float4 p:POSITION,inout sampler2D s):POSITION{return p+tex2Dlod(s,float4(p.xy,0,0));}",
     "sampler operand"),
    ("unknown_sampler_type", "fp",
     "float4 main(float4 p:TEXCOORD0,sampler2DShadow s):COLOR{return p;}",
     "unknown type"),
]
# Existing intrinsic gaps, measured separately from this storage rule. Both
# spellings must still fail by name, never turn into successful empty programs.
for kind, intrinsic, coordinate in (
        ("1D", "tex1Dlod", "float4(p.x,0,0,0)"),
        ("CUBE", "texCUBElod", "float4(p.xyz,0)")):
    for label, qualifier in (("in", "in "), ("uniform", "uniform ")):
        REFUSALS.append(("unsupported_" + kind + "_" + label, "vp",
                         "float4 main(float4 p:POSITION," + qualifier + "sampler" +
                         kind + " s):POSITION{return " + intrinsic + "(s," + coordinate + ");}",
                         "use of undeclared identifier '" + intrinsic + "'"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def records(blob):
    count, offset = struct.unpack_from(">2I", blob, 12)
    result = {}
    for i in range(count):
        row = struct.unpack_from(">12I", blob, offset + 48 * i)
        name = blob[row[4]:].split(bytes([0]), 1)[0].decode() if row[4] else ""
        result[name] = row
    return result


def check(blob, stage, samplers, units, resources):
    table = records(blob)
    for name, (type_id, unit, referenced) in samplers.items():
        require(name in table, "missing sampler " + name)
        row = table[name]
        expected = (type_id, 3256 if unit is None else 2048 + unit, 4102, 4097, referenced)
        actual = (row[0], row[1], row[2], row[8], row[10])
        require(actual == expected, "sampler %s reflection %s, want %s" % (name, actual, expected))
    for name, expected in resources.items():
        require(name in table, "missing varying/output " + name)
        row = table[name]
        require((row[1], row[2], row[10]) == expected,
                "wrong varying/output resource, domain or liveness for " + name)
    if stage == "fp":
        actual_units = [(w[0] >> 17) & 15 for w, _ in instructions(ucode_words(blob))
                        if (w[0] >> 24) & 63 in (0x17, 0x18, 0x19, 0x2F, 0x31)]
    else:
        header = struct.unpack_from(">8I", blob)
        words = [struct.unpack_from(">4I", blob, header[7] + 16 * i)
                 for i in range(header[6] // 16)]
        actual_units = [(w[2] >> 8) & 3 for w in words if (w[1] >> 22) & 31 == 0x19]
    require(sorted(actual_units) == sorted(units),
            "fetch units %s, want %s" % (actual_units, units))


def compile_one(compiler, work, name, stage, text):
    source, output = work / (name + ".cg"), work / (name + ".bin")
    source.write_text(text + "\n", encoding="utf-8")
    output.unlink(missing_ok=True)
    run = subprocess.run([compiler, "-p", "sce_" + stage + "_rsx",
                          "--emit-container", str(output), str(source)],
                         capture_output=True, text=True, timeout=30)
    log = run.stdout + run.stderr
    (work / (name + ".log")).write_text(log, encoding="utf-8")
    return run.returncode, output.read_bytes() if output.exists() else b"", log


def run_checks(compiler, work):
    failures = []
    for name, stage, template, samplers, units, resources in CASES:
        controls = None
        spellings = [("uniform", "uniform "), ("const", "const ")] if name == "const_input" else [
            ("uniform", "uniform "), ("none", ""), ("in", "in ")]
        for label, spelling in spellings:
            full_name = name + "_" + label
            try:
                rc, blob, log = compile_one(compiler, work, full_name, stage,
                                            template.replace("Qsampler", spelling + "sampler"))
                require(rc == 0 and bool(blob), "compile rc=%d: %s" % (rc, log.strip()[-600:]))
                check(blob, stage, samplers, units, resources)
                if label == "uniform":
                    controls = blob
                else:
                    require(controls is not None, "uniform control failed")
                    require(blob == controls, "input spelling changed complete container")
                print("checked " + full_name)
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                failures.append(full_name + ": " + str(error))
    for name, stage, text, diagnostic in REFUSALS:
        try:
            rc, blob, log = compile_one(compiler, work, name, stage, text)
            require(rc == 1 and not blob and diagnostic in log,
                    "expected named refusal, got rc=%d bytes=%d: %s" % (rc, len(blob), log.strip()[-600:]))
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append(name + ": " + str(error))
    for failure in failures:
        print("FAIL: " + failure)
    print("entry-sampler: " + ("FAIL (%d)" % len(failures) if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run_checks(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix="entry-sampler-") as directory:
        return run_checks(args.compiler, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())

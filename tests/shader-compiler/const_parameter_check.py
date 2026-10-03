"""Const helper parameters are read-only even in uncalled function bodies.

Native measurements establish C1036 for direct/compound/member writes and C1112
for const actuals passed to inout parameters. Const helper reads with literal,
varying, uniform and default arguments produce their plain-parameter twins.
Index/out and nested-shadow controls exercise the same binding rules.
Entry const input allocation and static-parameter extensions are separate work.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from entry_sampler_check import compile_one, require


def entry(stage, expression, prefix=""):
    source_semantic, output_semantic = ("TEXCOORD0", "COLOR") if stage == "fp" else ("POSITION", "POSITION")
    return prefix + "float4 main(float4 p:" + source_semantic + "):" + output_semantic + "{return " + expression + ";}"


# Helper source using Q for the const qualifier, called expression, diagnostic.
WRITES = [
    ("direct", "float4 change(Qbool b,float4 p){b=!b;return b?p:p.wzyx;}\n", "change(true,p)", "expression is not assignable"),
    ("compound", "float change(Qfloat x){x+=1;return x;}\n", "p+change(p.x)", "expression is not assignable"),
    ("member", "float4 change(Qfloat4 x){x.x=3;return x;}\n", "change(p)", "expression is not assignable"),
    ("index", "float4 change(Qfloat4 x){x[1]=3;return x;}\n", "change(p)", "expression is not assignable"),
    ("inout", "void mutate(inout bool b){b=!b;}\nfloat4 change(Qbool b,float4 p){mutate(b);return b?p:p.wzyx;}\n",
     "change(true,p)", "const qualified actual parameter"),
    ("out", "void mutate(out float x){x=3;}\nfloat change(Qfloat x){mutate(x);return x;}\n",
     "p+change(p.x)", "const qualified actual parameter"),
    ("member_out", "void mutate(out float x){x=3;}\nfloat4 change(Qfloat4 x){mutate(x.x);return x;}\n",
     "change(p)", "const qualified actual parameter"),
]


def positive_cases(stage):
    result = []
    for name, argument in (("literal", "true"), ("varying", "p.x>0"), ("uniform", "gate")):
        prefix = "uniform bool gate;\n" if name == "uniform" else ""
        helper = "float4 choose(float4 p,Qbool b){return b?p:p.wzyx;}\n"
        text = entry(stage, "choose(p," + argument + ")", prefix + helper)
        result.append((name, text.replace("Q", "const "), text.replace("Q", "")))
    helper = "float4 choose(float4 p,Qbool b=true){return b?p:p.wzyx;}\n"
    text = entry(stage, "choose(p)", helper)
    result.append(("default", text.replace("Q", "const "), text.replace("Q", "")))
    text = entry(stage, "p+change(p.x)",
                 "float change(const float x){{float x=3;x+=1;}return x;}\n")
    result.append(("mutable_shadow", text, entry(stage, "p+p.x")))
    return result


def run_checks(compiler, work):
    failures = []
    def compile_checked(name, stage, source, diagnostic=None):
        try:
            rc, blob, log = compile_one(compiler, work, name, stage, source)
            if diagnostic is None:
                require(rc == 0 and bool(blob), "expected acceptance; rc=%d: %s" % (rc, log.strip()[-700:]))
            else:
                require(rc == 1 and not blob and diagnostic in log,
                        "expected named refusal; rc=%d bytes=%d: %s" % (rc, len(blob), log.strip()[-700:]))
            print("checked " + name)
            return blob
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append(name + ": " + str(error))
            return None
    for stage in ("fp", "vp"):
        for name, helper, expression, diagnostic in WRITES:
            for context, result in (("called", expression), ("dormant", "p")):
                stem = stage + "_" + context + "_" + name
                compile_checked(stem + "_mutable", stage, entry(stage, result, helper.replace("Q", "")))
                compile_checked(stem + "_const", stage, entry(stage, result, helper.replace("Q", "const ")), diagnostic)
        for name, source, twin in positive_cases(stage):
            stem = stage + "_read_" + name
            a = compile_checked(stem + "_const", stage, source)
            b = compile_checked(stem + "_control", stage, twin)
            if a is not None and b is not None and a != b:
                failures.append(stem + ": const helper changed complete container")
    for failure in failures:
        print("FAIL: " + failure)
    print("const-parameter: " + ("FAIL (%d)" % len(failures) if failures else "PASS"))
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run_checks(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix="const-parameter-") as directory:
        return run_checks(args.compiler, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())

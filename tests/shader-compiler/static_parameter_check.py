"""Opt-in helper static parameters reduce to ordinary parameter byte twins.

The pinned bundled NVIDIA oracle accepts helper static/static const/const static
forms, keeps const writes illegal, and emits the static-free twins. Native SCE
rejects static parameters with C1064. Entry parameters remain outside this first
extension slice; crashes in the bundled oracle are not acceptance evidence.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from entry_sampler_check import require


FLAG = "--extension=static-parameters"
CASES = []


def entry(expression, stage="fp"):
    si, so = ("TEXCOORD0", "COLOR") if stage == "fp" else ("POSITION", "POSITION")
    return "float4 main(float4 p:" + si + "):" + so + "{return " + expression + ";}"


def pair(name, text, stage="fp"):
    CASES.append((name, stage, text, text.replace("static ", "")))


for name, qualifier in (("static", "static "), ("static_const", "static const "),
                        ("const_static", "const static ")):
    helper = "float4 choose(float4 p," + qualifier + "bool b){return b?p:p.wzyx;}\n"
    pair("literal_" + name, helper + entry("choose(p,true)"))
    pair("default_" + name,
         "float4 choose(float4 p," + qualifier + "bool b=true){return b?p:p.wzyx;}\n" + entry("choose(p)"))
pair("literal_vertex", "float4 choose(float4 p,static const bool b){return b?p:p.wzyx;}\n" + entry("choose(p,false)", "vp"), "vp")
pair("varying_vertex", "float4 choose(float4 p,static const bool b){return b?p:p.wzyx;}\n" + entry("choose(p,p.x>0)", "vp"), "vp")
pair("mutable_static", "float4 choose(float4 p,static bool b){b=!b;return b?p:p.wzyx;}\n" + entry("choose(p,true)"))
pair("inline_static_const", "float4 choose(float4 p,inline static const bool b){return b?p:p.wzyx;}\n" + entry("choose(p,true)"))
pair("prototype_orders",
     "float4 choose(float4 p,static const bool b);\n"
     "float4 choose(float4 p,const static bool b){return b?p:p.wzyx;}\n" + entry("choose(p,true)"))
pair("dormant_unknown_name", "float4 unused(static const bool b){return absent_name;}\n" + entry("p"))
pair("dormant_wrong_arity", "float4 unused(float4 p,static const bool b){return dot(p,p,p);}\n" + entry("p"))


REFUSALS = []
for qualifier in ("static const ", "const static "):
    label = qualifier.strip().replace(" ", "_")
    for context, expression in (("called", "change(p,true)"), ("dormant", "p")):
        REFUSALS.append((label + "_write_" + context,
            "float4 change(float4 p," + qualifier + "bool b){b=!b;return b?p:p.wzyx;}\n" + entry(expression),
            "expression is not assignable"))
REFUSALS += [
    ("const_copy_out", "void put(out float x){x=3;}\n"
     "float change(static const float x){put(x);return x;}\n" + entry("p+change(p.x)"),
     "const qualified actual parameter"),
    ("const_member_write", "float4 change(static const float4 x){x.x=3;return x;}\n" + entry("change(p)"),
     "expression is not assignable"),
    ("qualifier_overload", "float4 choose(float4 p,bool b){return p;}\n"
     "float4 choose(float4 p,static const bool b){return p.wzyx;}\n" + entry("choose(p,true)"),
     "differs only in parameter qualifiers"),
    ("prototype_qualifier_mismatch", "float4 choose(float4 p,static const bool b);\n"
     "float4 choose(float4 p,bool b){return p;}\n" + entry("choose(p,true)"),
     "differs only in parameter qualifiers"),
    ("prototype_qualifier_reverse", "float4 choose(float4 p,bool b);\n"
     "float4 choose(float4 p,static const bool b){return p;}\n" + entry("choose(p,true)"),
     "differs only in parameter qualifiers"),
    ("definition_then_prototype", "float4 choose(float4 p,static const bool b){return p;}\n"
     "float4 choose(float4 p,bool b);\n" + entry("p"),
     "differs only in parameter qualifiers"),
    ("repeated_prototypes", "float4 choose(float4 p,static const bool b);\n"
     "float4 choose(float4 p,bool b);\n" + entry("p"),
     "differs only in parameter qualifiers"),
    ("reached_unknown_name", "float4 change(float4 p,static const bool b){return absent_name;}\n" + entry("change(p,true)"),
     "absent_name"),
    ("reached_wrong_arity", "float4 change(float4 p,static const bool b){return dot(p,p,p);}\n" + entry("change(p,true)"),
     "no matching function"),
    ("entry_static", "float4 main(static float4 p:TEXCOORD0):COLOR{return p;}",
     "helper parameters only"),
]


def compile_one(compiler, work, name, stage, source, flags=()):
    src, output = work / (name + ".cg"), work / (name + ".bin")
    src.write_text(source + "\n", encoding="utf-8")
    output.unlink(missing_ok=True)
    run = subprocess.run([compiler, *flags, "-p", "sce_" + stage + "_rsx",
                          "--emit-container", str(output), str(src)],
                         capture_output=True, text=True, timeout=30)
    log = run.stdout + run.stderr
    (work / (name + ".log")).write_text(log, encoding="utf-8")
    return run.returncode, output.read_bytes() if output.exists() else b"", log


def run_checks(compiler, work):
    failures = []
    def run(name, stage, text, flags=(), diagnostic=None, forbidden=None):
        try:
            rc, blob, log = compile_one(compiler, work, name, stage, text, flags)
            if diagnostic:
                require(rc == 1 and not (work / (name + ".bin")).exists() and diagnostic in log,
                        "expected refusal %r; rc=%d bytes=%d: %s" % (diagnostic, rc, len(blob), log.strip()[-600:]))
            else:
                require(rc == 0 and bool(blob), "expected acceptance; rc=%d: %s" % (rc, log.strip()[-600:]))
            if forbidden:
                require(forbidden not in log, "unrelated syntax gained an extension hint")
            print("checked " + name)
            return blob
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append(name + ": " + str(error))
            return None
    for name, stage, source, twin in CASES:
        run(name + "_disabled", stage, source, diagnostic=FLAG)
        actual = run(name + "_enabled", stage, source, (FLAG,))
        expected = run(name + "_control", stage, twin)
        if actual is not None and expected is not None and actual != expected:
            failures.append(name + ": enabled output differs from static-free control")
    for name, source, diagnostic in REFUSALS:
        run(name + "_enabled", "fp", source, (FLAG,), diagnostic)
    # Ordinary declaration consistency is independent of the extension.
    # These dormant forms were measured on both profiles, including repeated
    # prototypes and a prototype that follows its definition.
    declarations = (
        ("const_plain", "float4 f(const bool b);float4 f(bool b){return 1;}", True),
        ("plain_const", "float4 f(bool b);float4 f(const bool b){return 1;}", True),
        ("repeat_const_plain", "float4 f(const bool b);float4 f(bool b);", True),
        ("repeat_plain_const", "float4 f(bool b);float4 f(const bool b);", True),
        ("definition_const_plain", "float4 f(const bool b){return 1;}float4 f(bool b);", True),
        ("definition_plain_const", "float4 f(bool b){return 1;}float4 f(const bool b);", True),
        ("repeat_const", "float4 f(const bool b);float4 f(const bool b);", False),
        ("const_match", "float4 f(const bool b);float4 f(const bool b){return 1;}", False),
        ("input_match", "float4 f(in bool b);float4 f(bool b){return 1;}", False),
        ("input_reverse", "float4 f(bool b);float4 f(in bool b){return 1;}", False),
        ("distinct_types", "float4 f(const bool b);float4 f(float b){return 1;}", False),
    )
    for stage in ("fp", "vp"):
        for name, declaration, refuses in declarations:
            run("ordinary_" + name + "_" + stage, stage, declaration + entry("p", stage),
                diagnostic="C1106" if refuses else None, forbidden=FLAG)
    for qualifier in ("static uniform", "static inout", "static static", "static const const"):
        name = qualifier.replace(" ", "_")
        source = "float4 f(" + qualifier + " float4 p){return p;}\n" + entry("f(p)")
        for label, flags in (("disabled", ()), ("enabled", (FLAG,))):
            run("unsupported_" + name + "_" + label, "fp", source, flags,
                diagnostic="unsupported static parameter qualifier combination", forbidden=FLAG)
    ordinary = "static const float K=2;\n" + entry("p*K")
    a = run("ordinary_static_global", "fp", ordinary)
    b = run("ordinary_static_global_enabled", "fp", ordinary, (FLAG,))
    if a is not None and b is not None and a != b:
        failures.append("extension changed ordinary static global")
    run("ordinary_const_error_no_hint", "fp", "float4 f(const bool b){b=false;return 1;}\n" + entry("f(true)"),
        diagnostic="expression is not assignable", forbidden=FLAG)
    selected = "float4 chosen(static float4 p:TEXCOORD0):COLOR{return p;}"
    for label, flags in (("disabled", ("-e", "chosen")), ("enabled", (FLAG, "-e", "chosen"))):
        run("selected_entry_" + label, "fp", selected, flags,
            diagnostic="helper parameters only", forbidden=FLAG)
    # 'main' can itself be a helper when -e selects another function.
    helper_main = ("float4 main(float4 p,static const bool b){return b?p:p.wzyx;}\n"
                   "float4 chosen(float4 p:TEXCOORD0):COLOR{return main(p,true);}")
    a = run("main_is_helper", "fp", helper_main, (FLAG, "-e", "chosen"))
    b = run("main_is_helper_control", "fp", helper_main.replace("static ", ""), ("-e", "chosen"))
    if a is not None and b is not None and a != b:
        failures.append("selected entry name changed helper normalization")
    listed = subprocess.run([compiler, "--list-extensions"], capture_output=True, text=True, timeout=30)
    if listed.returncode != 0 or "static-parameters" not in listed.stdout:
        failures.append("--list-extensions omits static-parameters")
    for failure in failures:
        print("FAIL: " + failure)
    print("static-parameter: " + ("FAIL (%d)" % len(failures) if failures else "PASS"))
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run_checks(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix="static-parameter-") as directory:
        return run_checks(args.compiler, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())

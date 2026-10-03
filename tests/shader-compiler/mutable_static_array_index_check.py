"""Initialized mutable static arrays retain bound-index values across helpers.

The source shapes were measured on both profiles. Function build order must not
turn a proven loop index into a runtime selector, or lose writes and shadowing.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate
from vp_binding_check import evaluate_bindings


PREFIX = "static float coeff[4]={0.125,0.25,0.5,1.0};\n"
LOOP = "float4 v=0; for(int i=0;i<3;i++) v+=t*coeff[i]; return v+t*coeff[3];"
IDENTITY = "float4 identity(float4 x) { return x; }\n"
INPUTS = [[0.5, 1.0, 2.0, 4.0], [-0.25, 0.125, 1.5, -2.0], [2.0, -3.0, 4.0, -5.0]]

# A preceding reachable helper clears the original module placeholder bindings.
# An unreachable helper is skipped, so that control catches a build-order leak.
CASES = {
    "entry_only": ("", LOOP),
    "helper_loop": ("float4 filter(float4 t) { " + LOOP + " }\n", "return filter(t);"),
    "prior_reached_helper": (IDENTITY, "t=identity(t); " + LOOP),
    "prior_dormant_helper": (IDENTITY, LOOP),
    "literal_twin": (
        IDENTITY, "t=identity(t); return t*(coeff[0]+coeff[1]+coeff[2]+coeff[3]);"),
    "postincrement": (
        IDENTITY, "t=identity(t); int i=0; float a=coeff[i++]; return float4(a,i,coeff[i],1);"),
    "written_element": (
        IDENTITY, "t=identity(t); coeff[1]=t.x; int i=1; return float4(coeff[i],coeff[0],coeff[2],1);"),
    "local_shadow": (
        IDENTITY, "t=identity(t); float coeff[4]={t.x,t.y,t.z,t.w}; int i=2; "
        "return float4(coeff[i],coeff[0],coeff[1],coeff[3]);"),
}
REFUSALS = {
    "dynamic": "t=identity(t); int i=int(t.x); return t*coeff[i];",
    "bound_high": "t=identity(t); int i=4; return t*coeff[i];",
    "bound_negative": "t=identity(t); int i=-1; return t*coeff[i];",
}


def expected(name, t):
    if name == "postincrement":
        return [0.125, 1.0, 0.25, 1.0]
    if name == "written_element":
        return [t[0], 0.125, 0.5, 1.0]
    if name == "local_shadow":
        return [t[2], t[0], t[1], t[3]]
    return [1.875 * value for value in t]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler")
    parser.add_argument("--keep", type=Path)
    args = parser.parse_args()
    failures = []
    checked = 0
    with tempfile.TemporaryDirectory(prefix="mutable-static-array-index-") as temporary:
        work = args.keep or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for stage in ("fp", "vp"):
            rows = {**CASES, **{name: (IDENTITY, body) for name, body in REFUSALS.items()}}
            for name, (helper, body) in rows.items():
                label = name + "_" + stage
                src, dst = work / (label + ".cg"), work / (label + ".bin")
                if dst.exists():
                    raise RuntimeError("output already exists: " + str(dst))
                output = "COLOR" if stage == "fp" else "POSITION"
                src.write_text(PREFIX + helper + "float4 selected(float4 t:TEXCOORD0):"
                               + output + " { " + body + " }\n", encoding="utf-8")
                run = subprocess.run(
                    [args.compiler, "-p", "sce_" + stage + "_rsx", "-e", "selected",
                     "--emit-container", str(dst), str(src)],
                    capture_output=True, text=True, timeout=30)
                src.with_suffix(".stdout").write_text(run.stdout, encoding="utf-8")
                src.with_suffix(".stderr").write_text(run.stderr, encoding="utf-8")
                try:
                    if name in REFUSALS:
                        assert run.returncode == 1 and not dst.exists(), run.stderr
                    else:
                        assert run.returncode == 0 and dst.is_file(), run.stderr
                        blob = dst.read_bytes()
                        for t in INPUTS:
                            value = (evaluate(blob, {"TEX0": t}) if stage == "fp"
                                     else evaluate_bindings(blob, {}, {"IN8": t})[0])
                            want = expected(name, t)
                            assert value == want, (t, value, want)
                            checked += 1
                    print("PASS", label)
                except (AssertionError, ValueError, RuntimeError, KeyError, IndexError) as error:
                    failures.append(label)
                    print("FAIL", label, str(error))
    print("mutable-static-array-index:", len(failures), "failures;", checked, "numeric checks")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())

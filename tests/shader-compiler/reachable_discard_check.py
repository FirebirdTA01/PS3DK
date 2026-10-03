"""Dormant discard stays legal; reached VP discard and dormant type errors refuse.

All eight source shapes were measured on both profiles. Dormant sources have
identity twins; reached fragment sources must retain a hardware KIL instruction.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from fp_sources import instructions, ucode_words


# name: (declarations before entry, entry body, declarations after entry)
CASES = {
    "dormant_sibling": (
        "float4 sibling(float4 q:TEXCOORD0):COLOR { if(q.x<0) discard; return q; }",
        "return t;", ""),
    "dormant_overload": (
        "float choose(float x) { if(x<0) discard; return x; } "
        "float4 choose(float4 x) { return x; }", "return choose(t);", ""),
    "direct": ("", "if(t.x<0) discard; return t;", ""),
    "reached_overload": (
        "float choose(float x) { if(x<0) discard; return 1; } "
        "float4 choose(float4 x) { return x; }", "return t*choose(t.x);", ""),
    "transitive": (
        "float leaf(float x) { if(x<0) discard; return 1; } "
        "float middle(float x) { return leaf(x); }", "return t*middle(t.x);", ""),
    "prototype_reached": (
        "float leaf(float x);", "return t*leaf(t.x);",
        "float leaf(float x) { if(x<0) discard; return 1; }"),
    "prototype_dormant": (
        "float leaf(float x);", "return t;",
        "float leaf(float x) { if(x<0) discard; return 1; }"),
    "dormant_type_error": (
        "float4 broken(float4 q) { float4 bad=float4(q.x,q.y); "
        "if(q.x<0) discard; return bad; }", "return t;", ""),
}
DORMANT = {"dormant_sibling", "dormant_overload", "prototype_dormant"}
REACHED = {"direct", "reached_overload", "transitive", "prototype_reached"}


def shader(case, vertex):
    before, body, after = case
    semantic, output = ("POSITION", "POSITION") if vertex else ("TEXCOORD0", "COLOR")
    # Explicit selection must work even when no function is called main.
    return (before + "\nfloat4 selected(float4 t:" + semantic + "):" + output
            + " { " + body + " }\n" + after + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler")
    parser.add_argument("--keep", type=Path)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="reachable-discard-") as temporary:
        work = args.keep or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)

        def compile_case(name, case, vertex):
            label = name + ("_vp" if vertex else "_fp")
            src, dst = work / (label + ".cg"), work / (label + ".bin")
            if dst.exists():
                raise RuntimeError("output already exists: " + str(dst))
            src.write_text(shader(case, vertex), encoding="utf-8")
            run = subprocess.run(
                [args.compiler, "-p", "sce_vp_rsx" if vertex else "sce_fp_rsx",
                 "-e", "selected", "--emit-container", str(dst), str(src)],
                capture_output=True, text=True, timeout=30)
            (work / (label + ".stdout")).write_text(run.stdout, encoding="utf-8")
            (work / (label + ".stderr")).write_text(run.stderr, encoding="utf-8")
            return run, dst

        for vertex in (False, True):
            twin, twin_path = compile_case("identity", ("", "return t;", ""), vertex)
            if twin.returncode != 0 or not twin_path.is_file():
                raise RuntimeError("identity control failed: " + twin.stderr)
            for name, case in CASES.items():
                label = name + ("_vp" if vertex else "_fp")
                run, dst = compile_case(name, case, vertex)
                try:
                    if name == "dormant_type_error" or (vertex and name in REACHED):
                        assert run.returncode == 1 and not dst.exists(), run.stderr
                        diagnostic = ("constructor requires 4 components" if name == "dormant_type_error"
                                      else "'discard' can only be used in fragment shaders")
                        assert diagnostic in run.stderr, run.stderr
                    else:
                        assert run.returncode == 0 and dst.is_file(), run.stderr
                        blob = dst.read_bytes()
                        if name in DORMANT:
                            assert blob == twin_path.read_bytes(), "dormant function changed the identity shader"
                        if not vertex:
                            kills = [w for w, _ in instructions(ucode_words(blob))
                                     if ((w[0] >> 24) & 0x3F) == 0x12]
                            assert bool(kills) == (name in REACHED), "reached discard was lost or dormant discard leaked"
                    print("PASS", label)
                except (AssertionError, RuntimeError, ValueError) as exc:
                    failures.append(label)
                    print("FAIL", label, str(exc))
    print("reachable-discard:", len(failures), "failures")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())

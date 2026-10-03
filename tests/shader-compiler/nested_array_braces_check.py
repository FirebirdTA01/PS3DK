"""Nested numeric brace initializers: byte twins, values, and shape boundaries."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile

import fp_eval

INPUTS = [[0.125, 0.25, 0.5, 0.75], [-0.5, 0.25, 1.5, 2.0], [2.0, 1.5, 0.25, -0.5]]


def source(body, prefix="", vertex=False):
    semantic = "POSITION" if vertex else "TEXCOORD0"
    output = "POSITION" if vertex else "COLOR"
    return prefix + f"float4 main(float4 t:{semantic}):{output} {{ {body} }}\n"


# Every brace spelling has an explicit constructor twin. Expected values are
# independent of either compiler; the byte check also covers VP without a VP evaluator.
CASES = {
    "local_vector": ("float2 a[2]={{t.x,t.y},{t.z,t.w}}; return float4(a[0],a[1]);",
                     "float2 a[2]={float2(t.x,t.y),float2(t.z,t.w)}; return float4(a[0],a[1]);", "", None),
    "unsized_vector": ("float2 a[]={{t.x,t.y},{t.z,t.w}}; return float4(a[0],a[1]);",
                       "float2 a[]={float2(t.x,t.y),float2(t.z,t.w)}; return float4(a[0],a[1]);", "", None),
    "mixed_vector": ("float2 a[2]={{t.x,t.y},t.zw}; return float4(a[0],a[1]);",
                     "float2 a[2]={float2(t.x,t.y),t.zw}; return float4(a[0],a[1]);", "", None),
    "matrix_rows": ("float2x2 a={{t.x,t.y},{t.z,t.w}}; return float4(a[0],a[1]);",
                    "float2x2 a=float2x2(t.x,t.y,t.z,t.w); return float4(a[0],a[1]);", "", None),
    "rectangular_rows": ("float2x3 a={{t.x,t.y,1},{t.z,t.w,2}}; return float4(a[0].xy,a[1].xy);",
                         "float2x3 a=float2x3(t.x,t.y,1,t.z,t.w,2); return float4(a[0].xy,a[1].xy);", "", None),
    "explicit_splat": ("float2 a[2]={float2(t.x),t.zw}; return float4(a[0].x,t.y,a[1]);",
                       "float2 a[2]={float2(t.x,t.x),t.zw}; return float4(a[0].x,t.y,a[1]);", "", None),
    "explicit_narrow": ("float2 a[2]={float2(t),t.zw}; return float4(a[0],a[1]);",
                        "float2 a[2]={t.xy,t.zw}; return float4(a[0],a[1]);", "", None),
    "half_elements": ("half2 a[2]={{t.x,t.y},{t.z,t.w}}; return float4(a[0],a[1]);",
                      "half2 a[2]={half2(t.x,t.y),half2(t.z,t.w)}; return float4(a[0],a[1]);", "", None),
    "side_effect_once": ("float2 a[2]={{step(),step()},{step(),step()}}; return float4(a[0]+a[1],n,1);",
                         "float2 a[2]={float2(step(),step()),float2(step(),step())}; return float4(a[0]+a[1],n,1);",
                         "static float n=0; float step(){ n+=0.25; return 0.25; } ", [0.5,0.5,1.0,1.0]),
    "global_vector": ("return float4(a[0],a[1]);", "return float4(a[0],a[1]);",
                      ("static const float2 a[2]={{0.125,0.25},{0.5,0.75}}; ",
                       "static const float2 a[2]={float2(0.125,0.25),float2(0.5,0.75)}; "), [0.125,0.25,0.5,0.75]),
}
REFUSE = {
    "underfilled": "float2 a[2]={{t.x},{t.z,t.w}}; return float4(a[0],a[1]);",
    "overfilled": "float2 a[2]={{t.x,t.y,t.z},{t.z,t.w}}; return float4(a[0],a[1]);",
    "outer_underfilled": "float2 a[2]={{t.x,t.y}}; return float4(a[0],a[1]);",
    "outer_overfilled": "float2 a[1]={{t.x,t.y},{t.z,t.w}}; return float4(a[0],0,1);",
    "no_narrowing": "float2 a[2]={{t},{t}}; return float4(a[0],a[1]);",
    "scalar_extra_braces": "float a[2]={{t.x},{t.y}}; return float4(a[0],a[1],t.z,t.w);",
    "matrix_row_underfilled": "float2x3 a={{t.x,t.y},{t.z,t.w,2}}; return float4(a[0].xy,a[1].xy);",
    "matrix_row_overfilled": "float2x3 a={{t.x,t.y,1,2},{t.z,t.w,2}}; return float4(a[0].xy,a[1].xy);",
}
# Supported syntax can still hit an explicit lowering boundary. Do not silently
# accept a matrix array without storage that can represent its elements.
DEBT = {"matrix_array": "float2x2 a[1]={{{t.x,t.y},{t.z,t.w}}}; return float4(a[0][0],a[0][1]);"}


def main():
    compiler = sys.argv[1]
    failures = []
    with contextlib.redirect_stdout(io.StringIO()):
        assert fp_eval.self_test()
    with tempfile.TemporaryDirectory(prefix="nested-array-braces-") as tmp:
        work = Path(tmp)
        def compile_row(name, text, vertex=False):
            src, dst = work/(name+".cg"), work/(name+".bin")
            src.write_text(text)
            p = subprocess.run([compiler, "-p", "sce_vp_rsx" if vertex else "sce_fp_rsx",
                                "--emit-container", str(dst), str(src)], capture_output=True, text=True, timeout=30)
            return p, dst
        for vertex in (False, True):
            for name, (braces, twin, prefix, expected) in CASES.items():
                if vertex and name == "side_effect_once":
                    continue
                label = name + ("_vp" if vertex else "_fp")
                prefixes = prefix if isinstance(prefix, tuple) else (prefix, prefix)
                pair = [compile_row(label+str(i), source(body, prefixes[i], vertex), vertex)
                        for i, body in enumerate((braces, twin))]
                try:
                    for p, dst in pair:
                        assert p.returncode == 0 and dst.is_file(), p.stderr
                    assert pair[0][1].read_bytes() == pair[1][1].read_bytes(), "brace/constructor bytes differ"
                    if not vertex:
                        for t in INPUTS:
                            got = fp_eval.evaluate(pair[0][1].read_bytes(), {"TEX0":t})
                            assert got == (t if expected is None else expected), (t, got, expected)
                    print("PASS", label)
                except (AssertionError, RuntimeError, ValueError) as e:
                    failures.append(label); print("FAIL", label, str(e))
        for vertex in (False, True):
            for name, body in {**REFUSE, **DEBT}.items():
                label = name + ("_vp" if vertex else "_fp")
                p, dst = compile_row(label, source(body, vertex=vertex), vertex)
                ok = p.returncode == 1 and not dst.exists()
                if name in DEBT:
                    ok = ok and "local-array-initialiser" in p.stderr
                print("PASS" if ok else "FAIL", label)
                if not ok: failures.append(label)
    print("nested-array-braces:", len(failures), "failures")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())

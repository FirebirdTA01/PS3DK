"""Tests for float literals, suffixes, swizzles, braced defaults, and attributes (Bucket b)."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


def compile_shader(compiler, work, name, profile, text, extra_args=None):
    src = work / (name + '.cg')
    out = work / (name + '.bin')
    src.write_text(text)
    cmd = [compiler, '-p', profile, '--dump-ir', '--emit-container', str(out), str(src)]
    if extra_args:
        cmd.extend(extra_args)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    assert p.returncode == 0 and out.is_file(), f"{name} failed (rc={p.returncode}): {p.stderr}\n{p.stdout}"
    blob = out.read_bytes()
    return blob, (p.stdout + p.stderr)


def main():
    if len(sys.argv) < 2:
        print("Usage: python float_literal_syntax_check.py <compiler-path>")
        sys.exit(1)
    compiler = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix='ps3dk-float-lit-') as temp:
        work = Path(temp)

        # 1. Leading dot float literal (.25f, .5) byte-equals 0.25f + 0.5 twin
        code1 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * .25f + .5;
}
"""
        code1_twin = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * 0.25f + 0.5;
}
"""
        blob1, _ = compile_shader(compiler, work, 'leading_dot', 'sce_fp_rsx', code1)
        blob1_twin, _ = compile_shader(compiler, work, 'leading_dot_twin', 'sce_fp_rsx', code1_twin)
        assert blob1 == blob1_twin, "leading dot float differs from 0.xx twin"
        print("PASS: leading dot float literals (.25f, .5) byte-equals 0.xx twin", flush=True)

        # 2. Trailing dot float literal (1.f, 2.) byte-equals 1.0f + 2.0 twin
        code2 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * 1.f + 2.;
}
"""
        code2_twin = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * 1.0f + 2.0;
}
"""
        blob2, _ = compile_shader(compiler, work, 'trailing_dot', 'sce_fp_rsx', code2)
        blob2_twin, _ = compile_shader(compiler, work, 'trailing_dot_twin', 'sce_fp_rsx', code2_twin)
        assert blob2 == blob2_twin, "trailing dot float differs from 1.0 twin"
        print("PASS: trailing dot float literals (1.f, 2.) byte-equals 1.0 twin", flush=True)

        # 3. Scientific notation (1e-3f, 2.5e+2) byte-equals decimal twin
        code3 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * 1e-3f + 2.5e+2;
}
"""
        code3_twin = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a * 0.001f + 250.0;
}
"""
        blob3, _ = compile_shader(compiler, work, 'scientific', 'sce_fp_rsx', code3)
        blob3_twin, _ = compile_shader(compiler, work, 'scientific_twin', 'sce_fp_rsx', code3_twin)
        assert blob3 == blob3_twin, "scientific notation float differs from decimal twin"
        print("PASS: scientific notation float literals (1e-3f, 2.5e+2) byte-equals decimal twin", flush=True)

        # 4. Literal swizzles (0.0.xxxx) byte-equals constructor twin
        code4 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    float4 z = 0.0.xxxx;
    return a + z;
}
"""
        code4_twin = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    float4 z = float4(0.0, 0.0, 0.0, 0.0);
    return a + z;
}
"""
        blob4, _ = compile_shader(compiler, work, 'literal_swizzle', 'sce_fp_rsx', code4)
        blob4_twin, _ = compile_shader(compiler, work, 'literal_swizzle_twin', 'sce_fp_rsx', code4_twin)
        assert blob4 == blob4_twin, "literal swizzle differs from constructor twin"
        print("PASS: literal swizzles (0.0.xxxx) byte-equals constructor twin", flush=True)

        # 5. Negative controls: hex, unsigned, malformed numbers, and attributes cleanly refuse with rc=1 and no container artifact
        for bad_code, bad_name, expected_reason in [
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * float(0xFF); }", "refuse_hex_ff", "hex integer literal"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * float(0x1F); }", "refuse_hex_1f", "hex integer literal"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * float(4294967295u / 2u); }", "refuse_unsigned", "unsigned integer literal"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * 1.xxxx; }", "refuse_int_swizzle", "integer literal with swizzle"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * 1.e; }", "refuse_exp_no_digits", "exponent with no digits (C0124)"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { return a * 1..5; }", "refuse_double_dot", "multiple decimal points"),
            ("float4 main(float4 a : TEXCOORD0, uniform float3 light = { 10.0f, 20.0f, 30.0f }) : COLOR { return a * light.x; }", "refuse_param_default_braced", "default parameter value (braced)"),
            ("float4 main(float4 a : TEXCOORD0, uniform float x = 1.0f) : COLOR { return a * x; }", "refuse_param_default_scalar", "default parameter value (scalar)"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { [frobnicate] if (a.x > 0.0) return a; return a; }", "refuse_frobnicate", "unknown statement attribute"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { [branch(] if (a.x > 0.0) return a; return a; }", "refuse_branch_paren", "malformed attribute syntax"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { float4 r = a; [unroll(2)] for (int i=0; i<2; ++i) r += a; return r; }", "refuse_unroll_arg", "attribute with argument"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { float4 r = a; [unroll] for (int i=0; i<2; ++i) r += a; return r; }", "refuse_unroll_for", "attribute on for loop"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { float4 r = a; [flatten] r.x = 0.0; return r; }", "refuse_flatten_expr", "attribute on expression statement"),
            ("float4 main(float4 a : TEXCOORD0) : COLOR { [] if (a.x > 0.0) return a; return a; }", "refuse_empty_attr", "empty attribute"),
        ]:
            src_bad = work / f"{bad_name}.cg"
            out_bad = work / f"{bad_name}.bin"
            src_bad.write_text(bad_code)
            p_bad = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out_bad), str(src_bad)],
                                   capture_output=True, text=True)
            assert p_bad.returncode == 1, f"{bad_name} ({expected_reason}) expected rc=1, got {p_bad.returncode}: {p_bad.stderr}"
            assert not out_bad.exists(), f"{bad_name} ({expected_reason}) emitted unexpected container on refusal"
        print("PASS: all negative controls cleanly refuse (rc=1, no container)", flush=True)

        # 6. Valid scalar swizzles on int, uint, bool, and parenthesized scalar
        code_scalar_swizzles = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    int i = 5;
    unsigned int u = 5;
    bool b = true;
    float4 r = a * float(i.x);
    r += a * float(u.x);
    r += a * float(b.x);
    r += a * float((1).x);
    return r;
}
"""
        blob_scalars, _ = compile_shader(compiler, work, 'scalar_swizzles', 'sce_fp_rsx', code_scalar_swizzles)
        print("PASS: valid scalar swizzles on int, uint, bool, and parenthesized integer", flush=True)

        # 7. Flow control statement attributes [branch], [flatten] on if statements
        code6 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    float4 r = a;
    [branch] if (a.x > 0.0) {
        r = a * 2.0;
    }
    [flatten] if (a.y > 0.0) {
        r = r + 1.0;
    }
    return r;
}
"""
        blob6, out6 = compile_shader(compiler, work, 'statement_attrs', 'sce_fp_rsx', code6)
        print("PASS: statement attributes ([branch], [flatten] on if statements)", flush=True)

        # 7. Additional accepted forms: .5.xxxx + p, 1.e5, .5h, p - -.25
        code7 = """
float4 main(float4 p : TEXCOORD0) : COLOR {
    float4 a = .5.xxxx + p;
    float4 b = p * 1.e5;
    half4 c = p * .5h;
    float4 d = p - -.25;
    return a + b + c + d;
}
"""
        blob7, out7 = compile_shader(compiler, work, 'extended_accepted_forms', 'sce_fp_rsx', code7)
        print("PASS: extended accepted forms (.5.xxxx, 1.e5, .5h, p - -.25)", flush=True)

        # 8. Unary plus operator (+a, +2.0f) byte-equals direct operand twin
        code8 = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return +a + +2.0f;
}
"""
        code8_twin = """
float4 main(float4 a : TEXCOORD0) : COLOR {
    return a + 2.0f;
}
"""
        blob8, _ = compile_shader(compiler, work, 'unary_plus', 'sce_fp_rsx', code8)
        blob8_twin, _ = compile_shader(compiler, work, 'unary_plus_twin', 'sce_fp_rsx', code8_twin)
        assert blob8 == blob8_twin, "unary plus differs from direct operand twin"
        print("PASS: unary plus operator (+a, +2.0f) byte-equals direct twin", flush=True)

        # 9. Real SDK samples regression if SDK available
        sdk_root = Path("C:/SDKs/Sony/SCE/PS3/475")
        if sdk_root.exists():
            sdk_refuse_tests = [
                ('duck_fp', 'sce_fp_rsx', 'samples/sdk/graphics/gcm/duck/fpshader.cg'),
                ('duck_vp', 'sce_vp_rsx', 'samples/sdk/graphics/gcm/duck/vpshader.cg'),
                ('report_main_mem', 'sce_fp_rsx', 'samples/sdk/graphics/gcm/report_to_main_memory/fpshader.cg'),
            ]
            for tname, prof, relpath in sdk_refuse_tests:
                fpath = sdk_root / relpath
                if fpath.exists():
                    out_bin = work / f'{tname}.bin'
                    cmd = [compiler, '-p', prof, '--emit-container', str(out_bin), str(fpath)]
                    p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
                    assert p.returncode == 1, f"expected rc=1 for {relpath} (default param value refusal), got {p.returncode}"
                    assert not out_bin.exists(), f"unexpected container emitted for {relpath}"
                    assert "default parameter values are not supported" in p.stderr, f"expected diagnostic for {relpath}: {p.stderr}"
                    print(f"PASS: SDK shader honest refusal (parameter defaults) {relpath}", flush=True)

            sdk_accept_tests = [
                ('fpclear', 'sce_fp_rsx', 'samples/edge/dxt-sample/fpclear.cg'),
                ('gauss1x7', 'sce_fp_rsx', 'samples/edge/post-sample/shaders/post_gauss1x7fp.cg'),
                ('gauss7x1', 'sce_fp_rsx', 'samples/edge/post-sample/shaders/post_gauss7x1fp.cg'),
            ]
            for tname, prof, relpath in sdk_accept_tests:
                fpath = sdk_root / relpath
                if fpath.exists():
                    out_bin = work / f'{tname}.bin'
                    cmd = [compiler, '-p', prof, '--emit-container', str(out_bin), str(fpath)]
                    p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
                    assert p.returncode == 0 and out_bin.is_file(), f"SDK shader {relpath} failed: {p.stderr}"
                    print(f"PASS: SDK shader regression {relpath}", flush=True)


if __name__ == '__main__':
    main()

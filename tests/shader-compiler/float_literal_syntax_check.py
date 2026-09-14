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

        # 5b. Parameter DEFAULTS are ACCEPTED, and the `f` suffix survives into
        # the recorded value.  These two spellings sat in the negative controls
        # above asserting rc=1, written when the parser refused every parameter
        # default; d5cb0e5b (t_4b54f26b A1) makes them legal and the REFERENCE
        # accepts both - measured against sce-cgc, `light` records [10,20,30] and
        # `x` records [1.0], byte-identical to ours.  The rows were a pinned
        # REFUSAL, not a property, so they are converted rather than deleted:
        # what this file actually cares about is that the suffix parses, and the
        # twin comparison says so without a container parser.  The NEAR-MISS row
        # is the half that matters - a dropped default would make the suffixed
        # and unsuffixed spellings equal for the wrong reason, and only a changed
        # VALUE moving the container proves the default reached it.  Both
        # surfaces and the exact reference values are pinned separately by
        # tests/shader-compiler/entry-param-default-test.sh.
        braced_f     = "float4 main(float4 a : TEXCOORD0, uniform float3 light = { 10.0f, 20.0f, 30.0f }) : COLOR { return a * light.x; }"
        braced_plain = "float4 main(float4 a : TEXCOORD0, uniform float3 light = { 10.0, 20.0, 30.0 }) : COLOR { return a * light.x; }"
        braced_near  = "float4 main(float4 a : TEXCOORD0, uniform float3 light = { 10.0f, 20.0f, 31.0f }) : COLOR { return a * light.x; }"
        scalar_f     = "float4 main(float4 a : TEXCOORD0, uniform float x = 1.0f) : COLOR { return a * x; }"
        scalar_plain = "float4 main(float4 a : TEXCOORD0, uniform float x = 1.0) : COLOR { return a * x; }"
        scalar_near  = "float4 main(float4 a : TEXCOORD0, uniform float x = 2.0f) : COLOR { return a * x; }"
        blob_bf, _ = compile_shader(compiler, work, 'param_default_braced_f', 'sce_fp_rsx', braced_f)
        blob_bp, _ = compile_shader(compiler, work, 'param_default_braced_plain', 'sce_fp_rsx', braced_plain)
        blob_bn, _ = compile_shader(compiler, work, 'param_default_braced_near', 'sce_fp_rsx', braced_near)
        assert blob_bf == blob_bp, "braced default: f-suffixed literals differ from the unsuffixed twin"
        assert blob_bf != blob_bn, "braced default: a changed component never reached the container"
        blob_sf, _ = compile_shader(compiler, work, 'param_default_scalar_f', 'sce_fp_rsx', scalar_f)
        blob_sp, _ = compile_shader(compiler, work, 'param_default_scalar_plain', 'sce_fp_rsx', scalar_plain)
        blob_sn, _ = compile_shader(compiler, work, 'param_default_scalar_near', 'sce_fp_rsx', scalar_near)
        assert blob_sf == blob_sp, "scalar default: 1.0f differs from the 1.0 twin"
        assert blob_sf != blob_sn, "scalar default: a changed value never reached the container"
        print("PASS: parameter defaults accepted; f suffix byte-equals its twin and a changed value moves the container", flush=True)

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
            # These three SDK shaders were pinned here as REFUSALS while the
            # parser rejected every parameter default.  d5cb0e5b (t_4b54f26b A1)
            # compiles all three, and the reference compiles them too - they are
            # three of the six reference-SDK rows that flipped refused->accepted
            # in the 920-row sweep for that slice.  A pinned refusal is not a
            # property, so they move to the accept list rather than being
            # deleted: the shaders are still real coverage, now of the behaviour
            # we actually want.
            sdk_accept_tests = [
                ('duck_fp', 'sce_fp_rsx', 'samples/sdk/graphics/gcm/duck/fpshader.cg'),
                ('duck_vp', 'sce_vp_rsx', 'samples/sdk/graphics/gcm/duck/vpshader.cg'),
                ('report_main_mem', 'sce_fp_rsx', 'samples/sdk/graphics/gcm/report_to_main_memory/fpshader.cg'),
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

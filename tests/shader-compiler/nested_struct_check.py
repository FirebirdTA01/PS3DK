"""Tests for nested struct member lookup, lvalue handling, and swizzle assignments (t_5386e484)."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fp_sources
import struct


def get_output_from_h0(blob):
    _, _, _, _, _, prog, _, _ = struct.unpack_from(">8I", blob, 0)
    _, h0, _, _ = struct.unpack_from(">4B", blob, prog + 18)
    return h0


def compile_shader(compiler, work, name, profile, text):
    src, out = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    p = subprocess.run([compiler, '-p', profile, '--dump-ir', '--emit-container',
                        str(out), str(src)],
                       capture_output=True, text=True, timeout=20)
    assert p.returncode == 0 and out.is_file(), f"{name} failed (rc={p.returncode}): {p.stderr}"
    blob = out.read_bytes()
    return blob, (p.stdout + p.stderr)


def main():
    if len(sys.argv) < 2:
        print("Usage: python nested_struct_check.py <compiler-path>")
        sys.exit(1)
    compiler = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix='ps3dk-nested-struct-') as temp:
        work = Path(temp)

        # 1. Fable's a4_nested.cg probe: swizzle write and read-back on nested struct
        code1_nested = """
struct I { float4 v; };
struct R { float4 p : COLOR; I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.v.xy = t.xy;
    r.i.v.zw = t.zw;
    r.p = r.i.v;
    return r;
}
"""
        code1_twin = """
struct R { float4 p : COLOR; };
R main(float4 t : TEXCOORD0) {
    R r;
    float4 v;
    v.xy = t.xy;
    v.zw = t.zw;
    r.p = v;
    return r;
}
"""
        blob1, out1 = compile_shader(compiler, work, 'a4_nested', 'sce_fp_rsx', code1_nested)
        blob1_twin, _ = compile_shader(compiler, work, 'a4_twin', 'sce_fp_rsx', code1_twin)
        assert blob1 == blob1_twin, "a4_nested binary differs from flat-vector twin"
        assert re.search(r'stout.*COLOR', out1, re.I), "a4_nested missing StoreOutput to COLOR"
        print("PASS: a4_nested swizzle write and read-back (byte-identical to twin)", flush=True)

        # 2. Vertex shader profile with POSITION output (byte-identical to twin)
        code2_nested = """
struct I { float4 v; };
struct R { float4 p : POSITION; I i; };
R main(float4 t : POSITION) {
    R r;
    r.i.v.xy = t.xy;
    r.i.v.zw = t.zw;
    r.p = r.i.v;
    return r;
}
"""
        code2_twin = """
struct R { float4 p : POSITION; };
R main(float4 t : POSITION) {
    R r;
    float4 v;
    v.xy = t.xy;
    v.zw = t.zw;
    r.p = v;
    return r;
}
"""
        blob2, out2 = compile_shader(compiler, work, 'vp_nested', 'sce_vp_rsx', code2_nested)
        blob2_twin, _ = compile_shader(compiler, work, 'vp_twin', 'sce_vp_rsx', code2_twin)
        assert blob2 == blob2_twin, "vp_nested binary differs from flat-vector twin"
        assert re.search(r'stout.*POSITION', out2, re.I), "vp_nested missing StoreOutput to POSITION"
        print("PASS: vertex shader nested struct output (byte-identical to twin)", flush=True)

        # 3. Whole-field write to nested struct member (byte-identical to twin)
        code3_nested = """
struct I { float4 v; };
struct R { float4 p : COLOR; I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.v = t;
    r.p = r.i.v;
    return r;
}
"""
        code3_twin = """
struct R { float4 p : COLOR; };
R main(float4 t : TEXCOORD0) {
    R r;
    float4 v = t;
    r.p = v;
    return r;
}
"""
        blob3, out3 = compile_shader(compiler, work, 'whole_write', 'sce_fp_rsx', code3_nested)
        blob3_twin, _ = compile_shader(compiler, work, 'whole_twin', 'sce_fp_rsx', code3_twin)
        assert blob3 == blob3_twin, "whole_write binary differs from flat-vector twin"
        print("PASS: whole-field write to nested struct member (byte-identical to twin)", flush=True)

        # 4. Deeply nested struct (3 levels: c.b.a.v swizzle write, byte-identical to twin)
        code4_nested = """
struct A { float4 v; };
struct B { A a; };
struct C { float4 p : COLOR; B b; };
C main(float4 t : TEXCOORD0) {
    C c;
    c.b.a.v.xy = t.xy;
    c.b.a.v.zw = t.zw;
    c.p = c.b.a.v;
    return c;
}
"""
        code4_twin = """
struct C { float4 p : COLOR; };
C main(float4 t : TEXCOORD0) {
    C c;
    float4 v;
    v.xy = t.xy;
    v.zw = t.zw;
    c.p = v;
    return c;
}
"""
        blob4, out4 = compile_shader(compiler, work, 'deep_nested', 'sce_fp_rsx', code4_nested)
        blob4_twin, _ = compile_shader(compiler, work, 'deep_twin', 'sce_fp_rsx', code4_twin)
        assert blob4 == blob4_twin, "deep_nested binary differs from flat-vector twin"
        print("PASS: deeply nested 3-level struct swizzle write (byte-identical to twin)", flush=True)

        # 5. Sibling instances do not alias each other (byte-identical to flat twin)
        code5_nested = """
struct I { float4 v; };
struct R { float4 p : COLOR; I i; };
R main(float4 t : TEXCOORD0) {
    R r1;
    R r2;
    r1.i.v = t;
    r2.i.v = t.wzyx;
    r1.p = r1.i.v + r2.i.v;
    return r1;
}
"""
        code5_twin = """
struct R { float4 p : COLOR; };
R main(float4 t : TEXCOORD0) {
    R r1;
    float4 v1 = t;
    float4 v2 = t.wzyx;
    r1.p = v1 + v2;
    return r1;
}
"""
        blob5, out5 = compile_shader(compiler, work, 'siblings', 'sce_fp_rsx', code5_nested)
        blob5_twin, _ = compile_shader(compiler, work, 'siblings_twin', 'sce_fp_rsx', code5_twin)
        assert blob5 == blob5_twin, "siblings binary differs from flat-vector twin"
        assert re.search(r'add vec4', out5), "siblings should emit vector add of distinct operands"
        print("PASS: separate sibling instances do not alias (byte-identical to flat twin)", flush=True)

        # 6. Untouched nested leaf and output control: untouched fields emit NO StoreOutput
        code6 = """
struct I { float4 v; float4 untouched_leaf; };
struct R { float4 p : COLOR; float4 unwritten_out : TEXCOORD1; I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.v = t;
    r.p = r.i.v;
    return r;
}
"""
        blob6, out6 = compile_shader(compiler, work, 'untouched', 'sce_fp_rsx', code6)
        assert re.search(r'stout.*COLOR', out6, re.I), "untouched control missing StoreOutput to COLOR"
        assert not re.search(r'stout.*TEXCOORD1', out6, re.I), "untouched output emitted unexpected StoreOutput"
        print("PASS: untouched leaves and untouched exports emit no spurious StoreOutput", flush=True)

        # 7. Negative control: genuinely non-existent member must refuse with clean error (rc=1, no artifact)
        code7 = """
struct I { float4 v; };
struct R { I i; };
float4 main(float4 t : TEXCOORD0) : COLOR {
    R r;
    return r.i.nonexistent;
}
"""
        src7 = work / 'bad_member.cg'
        out7 = work / 'bad_member.bin'
        src7.write_text(code7)
        p7 = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out7), str(src7)],
                            capture_output=True, text=True)
        assert p7.returncode == 1, f"bad_member expected exit code 1 (clean refusal), got {p7.returncode}"
        assert not out7.exists(), "bad_member must produce no container artifact on refusal"
        assert "no member named 'nonexistent' in 'I'" in p7.stderr, f"unexpected error: {p7.stderr}"
        print("PASS: negative control correctly refuses with rc=1, no artifact, and exact error", flush=True)

        # 8. Codex probe: nested struct semantic leaf in VP return
        code8 = """
struct I { float4 v : POSITION; };
struct R { I i; float4 keep : TEXCOORD0; };
R main(float4 t : POSITION) {
    R r;
    r.i.v = t;
    r.keep = t;
    return r;
}
"""
        blob8, out8 = compile_shader(compiler, work, 'nested_vp_export', 'sce_vp_rsx', code8)
        assert re.search(r'stout.*POSITION', out8, re.I), "nested_vp_export missing StoreOutput POSITION"
        assert re.search(r'stout.*TEXCOORD', out8, re.I), "nested_vp_export missing StoreOutput TEXCOORD"
        assert "warning: vertex shader does not output POSITION" not in out8, "spurious missing-POSITION warning"
        assert b'main.i.v' in blob8, "container param table missing main.i.v"
        assert b'main.keep' in blob8, "container param table missing main.keep"
        print("PASS: nested struct semantic leaves in VP return exported correctly", flush=True)

        # 9. Codex probe: nested struct semantic leaf in FP return with partial writes
        code9 = """
struct I { float4 v : COLOR; };
struct R { I i; float4 keep : COLOR1; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.v.xy = t.xy;
    r.i.v.zw = t.zw;
    r.keep = t;
    return r;
}
"""
        blob9, out9 = compile_shader(compiler, work, 'nested_fp_export', 'sce_fp_rsx', code9)
        assert re.search(r'stout.*COLOR\b', out9, re.I), "nested_fp_export missing StoreOutput COLOR"
        assert re.search(r'stout.*COLOR1', out9, re.I), "nested_fp_export missing StoreOutput COLOR1"
        assert b'main.i.v' in blob9, "container param table missing main.i.v"
        assert b'main.keep' in blob9, "container param table missing main.keep"
        print("PASS: nested struct semantic leaves in FP return with partial writes exported correctly", flush=True)

        # 10. Untouched nested leaf WITH semantic emits NO StoreOutput
        code10 = """
struct I { float4 v : POSITION; float4 untouched : TEXCOORD1; };
struct R { I i; float4 keep : TEXCOORD0; };
R main(float4 t : POSITION) {
    R r;
    r.i.v = t;
    r.keep = t;
    return r;
}
"""
        blob10, out10 = compile_shader(compiler, work, 'untouched_nested_semantic', 'sce_vp_rsx', code10)
        assert re.search(r'stout.*POSITION', out10, re.I), "missing StoreOutput POSITION"
        assert re.search(r'stout.*TEXCOORD', out10, re.I), "missing StoreOutput TEXCOORD0"
        assert not re.search(r'stout.*TEXCOORD1', out10, re.I), "untouched nested semantic emitted unexpected StoreOutput"
        print("PASS: untouched nested semantic leaf emits no StoreOutput", flush=True)

        # 11. Codex bank witness: all-nested with unwritten float COLOR1 forces float bank (R0, outputFromH0=0)
        code11_nested = """
struct I { half4 c : COLOR; float4 unused : COLOR1; };
struct R { I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.c = t;
    return r;
}
"""
        code11_flat = """
struct R { half4 c : COLOR; float4 unused : COLOR1; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.c = t;
    return r;
}
"""
        blob11, _ = compile_shader(compiler, work, 'both_nested_unwritten_float', 'sce_fp_rsx', code11_nested)
        blob11_twin, _ = compile_shader(compiler, work, 'flat_unwritten_float', 'sce_fp_rsx', code11_flat)
        w11 = fp_sources.ucode_words(blob11)
        w11_twin = fp_sources.ucode_words(blob11_twin)
        assert w11 == w11_twin, "both_nested_unwritten_float ucode differs from flat twin"
        insts11 = [fp_sources.render(w, i) for i, (w, _) in enumerate(fp_sources.instructions(w11))]
        assert any('dst=R0' in inst for inst in insts11), f"expected float bank dst=R0, got {insts11}"
        assert not any('dst=H0' in inst for inst in insts11), f"unexpected half bank dst=H0 in {insts11}"
        assert get_output_from_h0(blob11) == 0, f"both_nested_unwritten_float expected outputFromH0=0, got {get_output_from_h0(blob11)}"
        print("PASS: all-nested unwritten float selects float bank R0 / outputFromH0=0 (ucode matches flat twin)", flush=True)

        # 12. All-half nested control: retains half bank H0 (outputFromH0=1)
        code12_nested = """
struct I { half4 c : COLOR; };
struct R { I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.i.c = t;
    return r;
}
"""
        code12_flat = """
struct R { half4 c : COLOR; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.c = t;
    return r;
}
"""
        blob12, _ = compile_shader(compiler, work, 'nested_half', 'sce_fp_rsx', code12_nested)
        blob12_twin, _ = compile_shader(compiler, work, 'flat_half', 'sce_fp_rsx', code12_flat)
        w12 = fp_sources.ucode_words(blob12)
        w12_twin = fp_sources.ucode_words(blob12_twin)
        assert w12 == w12_twin, "nested_half ucode differs from flat twin"
        insts12 = [fp_sources.render(w, i) for i, (w, _) in enumerate(fp_sources.instructions(w12))]
        assert any('dst=H0' in inst for inst in insts12), f"expected half bank dst=H0, got {insts12}"
        assert not any('dst=R0' in inst for inst in insts12), f"unexpected float bank dst=R0 in {insts12}"
        assert get_output_from_h0(blob12) == 1, f"nested_half expected outputFromH0=1, got {get_output_from_h0(blob12)}"
        print("PASS: all-half nested struct correctly retains half bank H0 / outputFromH0=1", flush=True)

        # 13. Top-level half plus nested unwritten float: selects float bank R0
        code13 = """
struct I { float4 unused : COLOR1; };
struct R { half4 c : COLOR; I i; };
R main(float4 t : TEXCOORD0) {
    R r;
    r.c = t;
    return r;
}
"""
        blob13, _ = compile_shader(compiler, work, 'nested_unwritten_float', 'sce_fp_rsx', code13)
        w13 = fp_sources.ucode_words(blob13)
        insts13 = [fp_sources.render(w, i) for i, (w, _) in enumerate(fp_sources.instructions(w13))]
        assert any('dst=R0' in inst for inst in insts13), f"expected float bank dst=R0, got {insts13}"
        assert not any('dst=H0' in inst for inst in insts13), f"unexpected half bank dst=H0 in {insts13}"
        assert get_output_from_h0(blob13) == 0, f"nested_unwritten_float expected outputFromH0=0, got {get_output_from_h0(blob13)}"
        print("PASS: nested unwritten float beside top-level half selects float bank R0 / outputFromH0=0", flush=True)


if __name__ == '__main__':
    main()

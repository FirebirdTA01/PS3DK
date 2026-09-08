"""Dead texture readers disappear; observable fetches and discard survive."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from fp_sources import instructions, ucode_words


def main():
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
        root = Path(directory)
        def compile_body(name, body):
            src, out = root/(name+'.cg'), root/(name+'.bin')
            src.write_text('float4 main(float4 p:TEXCOORD0,uniform sampler2D a,uniform sampler2D b):COLOR{'+body+'}')
            run = subprocess.run([compiler,'-p','sce_fp_rsx','--emit-container',str(out),str(src)],capture_output=True,timeout=30)
            assert run.returncode == 0 and out.exists(), (name,run.returncode,run.stderr)
            words = ucode_words(out.read_bytes())
            assert words, (name,'empty program')
            return words
        cases = [
            ('unused','float4 dead=tex2D(b,p.xy);return tex2D(a,p.xy);','return tex2D(a,p.xy);'),
            ('overwritten','float4 c=tex2D(b,p.xy)*2;c=tex2D(a,p.xy);return c;','return tex2D(a,p.xy);'),
            ('projective','float4 dead=tex2Dproj(b,p);return tex2D(a,p.xy);','return tex2D(a,p.xy);'),
            ('transitive','float4 dead=tex2D(b,tex2D(a,p.xy).xy);return p;','return p;'),
        ]
        for name, body, twin in cases:
            assert compile_body(name,body) == compile_body(name+'_twin',twin), (name,'dead fetch differs from live twin')
        for name, body, count, kill in [
            ('two_live','return tex2D(a,p.xy)+tex2D(b,p.zw);',2,False),
            ('texture_kill','if(tex2D(b,p.xy).x<0.5)discard;return tex2D(a,p.xy);',2,True),
            ('alpha_kill','\n#pragma alphakill b\nfloat4 dead=tex2D(b,p.xy);return tex2D(a,p.xy);',2,False),
            ('upper_alpha_kill','\n#pragma ALPHAKILL b\nfloat4 dead=tex2D(b,p.xy);return tex2D(a,p.xy);',2,False),
        ]:
            ops = [(w[0]>>24)&63 for w,_ in instructions(compile_body(name,body))]
            assert ops.count(0x17) == count, (name,'live fetch lost',ops)
            if kill: assert 0x12 in ops, (name,'discard lost',ops)
    print('dead-texture: PASS (dead chains match twins; live fetches and discard retained)')


if __name__ == '__main__':
    main()

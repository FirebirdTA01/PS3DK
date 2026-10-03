"""Exercise renumbering through emitted code, including H-to-R widening."""
import contextlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import fp_eval
from fp_sources import instructions, ucode_words, source, TEMP, ARITY

def shader(n, shape):
    body = ''.join(f'float s{i}=step({(i+1)/100:.2f},c.x+c.y*.001);' for i in range(n))
    body += 'float v=(' + '+'.join(f's{i}' for i in range(n)) + f')/{n}.0;'
    body += 'float w=(' + '+'.join(f's{i}*v' for i in range(n)) + f')/{n}.0;'
    if shape == 'depth':
        return 'void main(float4 c:TEXCOORD0,out float4 o:COLOR,out float z:DEPTH){'+body+'o=float4(w,w,w,1);z=w;}'
    if shape == 'select':
        body += 'if(c.z>0) return float4(w,w,w,1); return float4(1-w,1-w,1-w,1);'
    else:
        body += 'return float4(w,w,w,1);'
    return ('half4' if shape == 'half' else 'float4')+' main(float4 c:TEXCOORD0):COLOR{'+body+'}'

def main():
    with contextlib.redirect_stdout(io.StringIO()):
        assert fp_eval.self_test()
    with tempfile.TemporaryDirectory(prefix='fp-register-renumber-') as directory:
        directory = Path(directory)
        for shape in ('float','half','depth','select'):
            src=directory/(shape+'.cg'); dst=directory/(shape+'.bin')
            src.write_text(shader(24,shape))
            p=subprocess.run([sys.argv[1],'-p','sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            assert p.returncode==0 and dst.exists(), (shape,p.stdout,p.stderr)
            blob=dst.read_bytes(); highest=-1
            for words,_ in instructions(ucode_words(blob)):
                if not (words[0] & (1<<30)):
                    raw=(words[0]>>1)&63
                    assert raw<48,(shape,'destination',raw)
                    highest=max(highest,raw>>1 if words[0]&(1<<7) else raw)
                op=(words[0]>>24)&63
                for slot in range(1,ARITY.get(op,0)+1):
                    s=source(words,slot)
                    if s['type']==TEMP: assert s['reg']<48,(shape,'source',s)
            prog=struct.unpack_from('>I',blob,20)[0]
            assert blob[prog+18]==max(2,highest+1)<48
            for k in (0,6,12,18,24):
                w=(k/24)**2
                for z in (-1,1):
                    expected=1-w if shape=='select' and z<0 else w
                    actual=fp_eval.evaluate(blob,{'TEX0':[(k+.5)/100,0,z,1]})
                    assert actual==[expected,expected,expected,1],(shape,k,z,actual,expected)
            print('PASS',shape,'register fields and10 exact values')
        # More than24 half-used whole slots cannot fit. Keep the safety refusal.
        src=directory/'overflow.cg'; dst=directory/'overflow.bin';src.write_text(shader(40,'float'))
        p=subprocess.run([sys.argv[1],'-p','sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
        assert p.returncode==1 and not dst.exists() and 'usable FP temp index limit of 47' in p.stderr+p.stdout
        print('PASS unrepresentable allocation retains named refusal')
    return 0
if __name__=='__main__': raise SystemExit(main())

"""Static const array reads through bound integer indices retain values."""
from pathlib import Path
import contextlib
import io
import subprocess
import sys
import tempfile
import fp_eval

PREFIX = 'static const float k[3]={0.25,0.5,0.125}; '
CASES = {
    'loop': ('float4 a=0; for(int i=0;i<3;i++) a+=t*k[i]; return a;', [0.109375,0.21875,0.4375,0.65625]),
    'bound_local': ('int i=1; return t*k[i];', [0.0625,0.125,0.25,0.375]),
    'side_effect_index': ('int i=0; float a=k[i++]; return float4(a,i,k[i],1);', [0.25,1,0.5,1]),
    'shadowed_array': ('float k[3]={t.x,t.y,t.z}; int i=1; return float4(k[i],t.y,t.z,t.w);', [0.25,0.25,0.5,0.75]),
}
TWINS = {
    'loop': 'float4 a=0; a+=t*k[0]; a+=t*k[1]; a+=t*k[2]; return a;',
    'bound_local': 'return t*k[1];',
    'side_effect_index': 'return float4(0.25,1,0.5,1);',
    'shadowed_array': 'return float4(t.y,t.y,t.z,t.w);',
}
REFUSALS = {
    'out_of_range': 'int i=3; return t*k[i];',
    'negative': 'int i=-1; return t*k[i];',
    'dynamic': 'int i=int(t.x); return t*k[i];',
}
def shader(body,vertex=False):
    return PREFIX + 'float4 main(float4 t:'+('POSITION' if vertex else 'TEXCOORD0')+'):'+('POSITION' if vertex else 'COLOR')+' {'+body+'}\n'
def main():
    failures=[]
    with contextlib.redirect_stdout(io.StringIO()): assert fp_eval.self_test()
    with tempfile.TemporaryDirectory(prefix='static-array-loop-') as tmp:
        tmp=Path(tmp)
        for vertex in (False,True):
            for name,case in {**CASES,**{k:(v,None) for k,v in REFUSALS.items()}}.items():
                body,expected=case; label=name+('_vp' if vertex else '_fp')
                src=tmp/(label+'.cg');dst=tmp/(label+'.bin');src.write_text(shader(body,vertex))
                p=subprocess.run([sys.argv[1],'-p','sce_vp_rsx' if vertex else 'sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                ok=p.returncode==1 and not dst.exists() if expected is None else p.returncode==0 and dst.exists()
                if ok and expected is not None and not vertex:
                    ok=fp_eval.evaluate(dst.read_bytes(),{'TEX0':[.125,.25,.5,.75]})==expected
                if ok and vertex and expected is not None:
                    twin=tmp/'literal.cg'; twin.write_text(shader(TWINS[name],True)); twinbin=tmp/'literal.bin'
                    q=subprocess.run([sys.argv[1],'-p','sce_vp_rsx','--emit-container',str(twinbin),str(twin)],capture_output=True,text=True,timeout=30)
                    ok=q.returncode==0 and twinbin.read_bytes()==dst.read_bytes()
                print('PASS' if ok else 'FAIL',label)
                if not ok: failures.append(label);print(p.stderr)
            # A non-static const retains its existing refusal. A true uniform
            # must keep its parameter records and match the literal-index load.
            for storage in ('const','uniform'):
                label=storage+('_vp' if vertex else '_fp')
                pair=[]
                for n,body in enumerate(('int i=1; return t*k[i];','return t*k[1];')):
                    declaration = 'const float k[3]={0.25,0.5,0.125};' if storage=='const' else 'uniform float k[3];'
                    text=shader(body,vertex).replace('static const float k[3]={0.25,0.5,0.125};',declaration)
                    src=tmp/(label+str(n)+'.cg');dst=tmp/(label+str(n)+'.bin');src.write_text(text)
                    p=subprocess.run([sys.argv[1],'-p','sce_vp_rsx' if vertex else 'sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                    pair.append((p.returncode,dst))
                if storage=='uniform':
                    ok=all(rc==0 and dst.exists() for rc,dst in pair)
                    if ok:ok=pair[0][1].read_bytes()==pair[1][1].read_bytes() and b'k[1]\0' in pair[0][1].read_bytes()
                else:
                    ok=all(rc==1 and not dst.exists() for rc,dst in pair)
                print('PASS' if ok else 'FAIL',label)
                if not ok:failures.append(label)
    print('static-array-loop:',len(failures),'failures')
    return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())

"""Execute float3/float4 VP normalize, including a DP4-to-DP3 negative control."""
import argparse
import math
from pathlib import Path
import struct
import subprocess
import tempfile

from vp_pow_vector_check import evaluate

CASES={
    'uniform3':('U.xyz',3,False),
    'attribute3':('n.xyz',3,False),
    'negated3':('-U.zyx',3,False),
    'uniform4':('U',4,False),
    'attribute4':('n',4,False),
    'negated4':('-U.wzyx',4,False),
    'absolute4':('abs(U)',4,False),
    'truncated4':('U',4,True),
}
INPUTS=[[1.,2.,2.,4.],[-2.,3.,-6.,2.],[.25,-.5,.75,-1.]]

def source(name):
    expr,width,truncated=CASES[name]
    output='float4(q.xyz,1.)' if width==3 or truncated else 'q'
    return ('void main(float4 p:POSITION,float4 n:NORMAL,uniform float4 U,'
            'out float4 op:POSITION,out float4 color:COLOR0){'
            f'op=p;float{width} q=normalize({expr});color={output};}}\n')

def judge(blob,name):
    expr,width,truncated=CASES[name]
    for vector in INPUTS:
        if name.startswith('negated'):
            v=[-x for x in reversed(vector[:3] if width==3 else vector)]
        elif name=='absolute4':v=[abs(x) for x in vector]
        else:v=vector[:width]
        length=math.sqrt(sum(x*x for x in v))
        want=[x/length for x in v]
        if width==3 or truncated:want=want[:3]+[1.]
        outputs=evaluate(blob,{'U':vector},{0:[.25,.5,.75,1.],2:vector},binary32=True)
        assert outputs.get(0)==[.25,.5,.75,1.], 'POSITION changed'
        got=outputs.get(1)
        assert got and all(x is not None and math.isfinite(x) for x in got), 'missing/nonfinite COLOR0'
        assert all(abs(x-y)<2e-6 for x,y in zip(got,want)), (name,vector,got,want)

def wrong_width(blob):
    """Restore the old omitted-w reduction without changing other words."""
    out=bytearray(blob)
    size,off=struct.unpack_from('>II',blob,24)
    changed=0
    for pos in range(off,off+size,16):
        word=struct.unpack_from('>I',blob,pos+4)[0]
        if (word>>22)&31 == 7:
            struct.pack_into('>I',out,pos+4,(word&~(31<<22))|(5<<22))
            changed+=1
    assert changed==1, 'expected exactly one normalization DP4'
    return bytes(out)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('compiler',nargs='?')
    parser.add_argument('--write-sources',type=Path)
    parser.add_argument('--reference-dir',type=Path)
    args=parser.parse_args()
    if args.write_sources:
        args.write_sources.mkdir(parents=True,exist_ok=True)
        for name in CASES:(args.write_sources/(name+'.cg')).write_text(source(name))
        return
    assert args.compiler or args.reference_dir
    passed=0;failures=[];mutants=0
    with tempfile.TemporaryDirectory(prefix='vp-normalize-width-') as tmp:
        for name in CASES:
            try:
                if args.compiler:
                    src,dst=Path(tmp)/(name+'.cg'),Path(tmp)/(name+'.vpo')
                    src.write_text(source(name))
                    run=subprocess.run([args.compiler,'-p','sce_vp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                    assert run.returncode==0 and dst.exists(),run.stderr
                else:dst=args.reference_dir/(name+'.vpo')
                blob=dst.read_bytes()
                judge(blob,name)
                if CASES[name][1]==4:
                    mutant=wrong_width(blob)
                    try:judge(mutant,name)
                    except AssertionError:mutants+=1
                    else:raise AssertionError('omitted-w mutant was accepted')
                passed+=1
            except (AssertionError,OSError) as error:failures.append(name+': '+str(error))
    for failure in failures:print('FAIL:',failure)
    print(f'VP normalize width: tests={len(CASES)} pass={passed} fail={len(failures)}; omitted-w mutants rejected={mutants}')
    raise SystemExit(bool(failures))

if __name__=='__main__':main()

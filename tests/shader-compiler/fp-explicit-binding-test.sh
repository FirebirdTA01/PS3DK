#!/usr/bin/env bash
# The binding changes reflection, never FP inline constants or their offsets.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" <<'PY'
from pathlib import Path
import struct,subprocess,sys,tempfile
compiler=str(Path(sys.argv[1]).resolve());failures=[]
def require(ok,message):
    if not ok:failures.append(message)
def parse(blob):
    h=struct.unpack_from('>8I',blob);records={};offsets={};defaults={}
    def text(off):return blob[off:blob.index(0,off)].decode() if off else ''
    for i in range(h[3]):
        r=struct.unpack_from('>12I',blob,h[4]+48*i)
        key=(text(r[4]),r[9])
        records[key]=(r[0],r[1],r[3],text(r[7]),r[10],r[11])
        if r[6]:
            count=struct.unpack_from('>I',blob,r[6])[0]
            offsets[key]=blob[r[6]:r[6]+4+4*count]
        defaults[key]=list(struct.unpack_from('>4f',blob,r[5])) if r[5] else None
    return records,blob[h[7]:h[7]+h[6]],offsets,defaults
def record(typ,reg,sem,used=True,parent=False):
    return (typ,2178 if used and not parent else 3256,reg if used and not parent else 0xffffffff,sem,int(used),int(used))
with tempfile.TemporaryDirectory(prefix='ps3dk-fp-binding-') as td:
    root=Path(td)
    def run(name,source,expected,defaults=None,legacy=False,refusal=False,compact=False):
        parsed=[]
        for bound in (True,False):
            src=root/(name+str(bound)+'.cg');out=src.with_suffix('.bin')
            src.write_text(source if bound else source.replace(':register(C9)','').replace(':C9','').replace(':C009','').replace(':C256','').replace(':C255',''))
            result=subprocess.run([compiler,'-p','sce_fp_rsx','--legacy-lowering' if legacy else '--general-lowering','--emit-container',str(out),str(src)],capture_output=True,text=True,timeout=20)
            if refusal:
                require(result.returncode==1 and 'overlapping used FP constant bindings' in result.stderr and 't_642131af' in result.stderr,
                        name+': expected named interim alias refusal, got '+str(result.returncode)+' '+result.stderr)
                return
            if result.returncode!=0:
                failures.append(name+': compile exit'+str(result.returncode)+' '+result.stderr);return
            parsed.append(parse(out.read_bytes()))
        if compact:
            containers=[]
            for bound in (True,False):
                src=root/(name+str(bound)+'.cg');out=src.with_suffix('.cgb')
                result=subprocess.run([compiler,'-p','sce_fp_rsx','--emit-cgb-container',str(out),str(src)],capture_output=True,text=True,timeout=20)
                require(result.returncode==0,name+': compact compile failed '+result.stderr)
                if result.returncode==0:containers.append(out.read_bytes())
            require(len(containers)==2 and containers[0]==containers[1],name+': binding changed compact CGB')
        records,ucode,offsets,values=parsed[0]
        require(ucode==parsed[1][1],name+': binding changed instruction/inline-constant bytes')
        require(offsets==parsed[1][2],name+': binding changed embedded-constant relocation bytes')
        require(values==parsed[1][3],name+': binding changed compiled defaults')
        for key,want in expected.items():require(records.get(key)==want,name+': '+str(key)+' '+str(records.get(key))+' != '+str(want))
        for key,want in (defaults or {}).items():require(values.get(key)==want,name+': wrong default '+str(key)+' '+str(values.get(key)))
    # C<N> on a sampler is a different binding family. Preserve its existing
    # automatic unit and compact resource shape pending t_53a99605.
    for spelling in ('C9','register(C9)'):
        run('sampler-'+spelling,'uniform sampler2D s:'+spelling+'; float4 main(float2 p:TEXCOORD0):COLOR{return tex2D(s,p);}',
            {('s',0xffffffff):(1066,2048,0xffffffff,'',1,0)},compact=True)
    for used in ('u','v'):
        run('unused-alias-'+used,'uniform float4 u:C9; uniform float4 v:C9; float4 main(float4 p:TEXCOORD0):COLOR{return p*'+used+';}',
            {(n,0xffffffff):record(1048,9,'C9',n==used) for n in ('u','v')})
    run('alias','uniform float4 u:C9; uniform float4 v:C9; float4 main(float4 p:TEXCOORD0):COLOR{return p*u+v;}',{},refusal=True)
    run('matrix-overlap','uniform float3x3 M:C9; uniform float4 v:C10; float4 main(float4 p:TEXCOORD0):COLOR{return float4(mul(M,p.xyz),p.w)+v;}',{},refusal=True)
    run('array-overlap','uniform float4 u[2]:C9; uniform float4 v:C10; float4 main(float4 p:TEXCOORD0):COLOR{return p*u[1]+v;}',{},refusal=True)
    for spelling in ('C9','register(C9)'):
      for scope in ('global','entry'):
        pno=0xffffffff if scope=='global' else 1
        decl='uniform float4 u:'+spelling
        source=decl+'; float4 main(float4 p:TEXCOORD0):COLOR{return p*u;}' if scope=='global' else 'float4 main(float4 p:TEXCOORD0,'+decl+'):COLOR{return p*u;}'
        run(scope+spelling,source,{('u',pno):record(1048,9,'C9')})
        if scope=='global':run(scope+spelling+'-legacy',source,{('u',pno):record(1048,9,'C9')},legacy=True)
        for width,typ in [(3,1059),(4,1064)]:
            decl=f'uniform float{width}x{width} u:'+spelling
            expr='mul(u,p)' if width==4 else 'float4(mul(u,p.xyz),p.w)'
            source=decl+'; float4 main(float4 p:TEXCOORD0):COLOR{return '+expr+';}' if scope=='global' else 'float4 main(float4 p:TEXCOORD0,'+decl+'):COLOR{return '+expr+';}'
            expected={('u',pno):record(typ,9,'C9',parent=True)}
            for row in range(width):expected[(f'u[{row}]',pno)]=record(1044+width,9+row,'C9')
            run(scope+spelling+str(width),source,expected)
        decl='uniform float4 u[2]:'+spelling
        source=decl+'; float4 main(float4 p:TEXCOORD0):COLOR{return p*u[1];}' if scope=='global' else 'float4 main(float4 p:TEXCOORD0,'+decl+'):COLOR{return p*u[1];}'
        run(scope+spelling+'array',source,{('u[0]',pno):record(1048,9,'C9',False),('u[1]',pno):record(1048,10,'C9')})
    run('unused','uniform float4 u:C9; float4 main(float4 p:TEXCOORD0):COLOR{return p;}',{('u',0xffffffff):record(1048,9,'C9',False)})
    run('unused-matrix','uniform float3x3 u:C9; float4 main(float4 p:TEXCOORD0):COLOR{return p;}',
        {('u',0xffffffff):record(1059,9,'C9',False,parent=True),**{(f'u[{i}]',0xffffffff):record(1047,9+i,'C9',False) for i in range(3)}})
    run('above-vp-limit','uniform float3 u:C256=float3(2,3,5); float4 main(float4 p:TEXCOORD0):COLOR{return float4(p.xyz*u,p.w);}',
        {('u',0xffffffff):record(1047,256,'C256')},{('u',0xffffffff):[2,3,5,0]})
    run('leading-zero-semantic','uniform float4 u:C009; float4 main(float4 p:TEXCOORD0):COLOR{return p*u;}',
        {('u',0xffffffff):record(1048,9,'C009')})
    run('matrix-default','uniform float3x3 u:C9=float3x3(1,2,3,4,5,6,7,8,9); float4 main(float4 p:TEXCOORD0):COLOR{return float4(mul(u,p.xyz),p.w);}',
        {('u',0xffffffff):record(1059,9,'C9',parent=True),**{(f'u[{i}]',0xffffffff):record(1047,9+i,'C9') for i in range(3)}},
        {(f'u[{i}]',0xffffffff):[1+3*i,2+3*i,3+3*i,0] for i in range(3)})
if failures:
    for message in failures:print('FAIL: '+message,file=sys.stderr)
    sys.exit(1)
print('PASS: fp-explicit-binding (exact records, unchanged ucode/relocations/defaults, legacy, FP C256)')
PY

#!/usr/bin/env bash
# PS3_475 oracle rows: local build/binding-containment/oracle/results.json.
# Explicit pins occupy c0..255, independently of automatic c256-up/c467-down.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" "$repo_root/tests/shader-compiler" <<'PY'
from pathlib import Path
import subprocess,sys,tempfile
sys.path.insert(0,sys.argv[2])
from vp_binding_check import container,evaluate_bindings
compiler=str(Path(sys.argv[1]).resolve())
failures=[]
p=[1,-2,3,0.5]
def require(ok,text):
    if not ok:failures.append(text)
def matrix(element,width):
    return [[10*(element+1)+row*3+col/4 for col in range(width)] for row in range(width)]
def product(m,v):return [sum(a*b for a,b in zip(row,v)) for row in m]
# A record expectation: name -> (type, register or -1, original semantic,
# parameter number, shared). An unused record must still carry the semantic.
def rows(name,width,base,semantic,pno=0xffffffff):
    result={name:(1064 if width==4 else 1059,base,semantic,pno,int(base>=0 and bool(semantic)))}
    for row in range(width):result[f'{name}[{row}]']=(1044+width,base+row if base>=0 else -1,semantic,pno,int(base>=0 and bool(semantic)))
    return result
with tempfile.TemporaryDirectory(prefix='ps3dk-vp-binding-') as directory:
    root=Path(directory)
    def run(name,source,expected,constants=None,value=None,index=0.75,legacy=False,refusal=False):
        src,out=root/(name+'.cg'),root/(name+'.bin');src.write_text(source)
        try:
            result=subprocess.run([compiler,'-p','sce_vp_rsx','--legacy-lowering' if legacy else '--general-lowering',
                '--emit-container',str(out),str(src)],capture_output=True,text=True,timeout=20)
        except subprocess.TimeoutExpired:
            failures.append(name+': timeout');return
        if refusal:
            require(result.returncode==1 and 'explicit constant binding' in result.stderr and 'c0..c255' in result.stderr,
                    name+': expected named range refusal, got '+str(result.returncode)+' '+result.stderr)
            return
        if result.returncode!=0 or not out.exists():
            failures.append(name+': compile failed '+result.stderr);return
        blob=out.read_bytes()
        try:
            records,_=container(blob)
            for key,(typ,reg,sem,pno,shared) in expected.items():
                record_name=key[0] if isinstance(key,tuple) else key
                row=records.get((record_name,pno))
                if row is None:failures.append(name+': missing '+str(key));continue
                text=blob[row[7]:blob.index(0,row[7])].decode() if row[7] else ''
                got=(row[0],row[1],row[3],text,row[9],row[10],row[11])
                want=(typ,2178 if reg>=0 else 3256,reg if reg>=0 else 0xffffffff,sem,pno,int(reg>=0),shared)
                require(got==want,name+': '+str(key)+' '+str(got)+' != '+str(want))
            if value is not None:
                actual,reads=evaluate_bindings(blob,constants,{'IN0':p,'IN8':[index]*4})
                require(actual==value,name+': numeric '+str(actual)+' != '+str(value)+' reads '+str(reads))
        except (ValueError,KeyError,IndexError) as error:failures.append(name+': '+str(error))

    # Same visible name, different declaration owners: neither usage nor pin
    # metadata may leak from the selected entry parameter to the unused global.
    run('shadow-input','uniform float4 u:C9; float4 main(float4 u:POSITION):POSITION{return u;}',
        {('u',0xffffffff):(1048,-1,'C9',0xffffffff,0)}, {},p)
    for binding,reg,sem in [('',467,''),(':C12',12,'C12')]:
        run('shadow-uniform-'+str(reg),'uniform float4 u:C9; float4 main(float4 p:POSITION,uniform float4 u'+binding+'):POSITION{return p*u;}',
            {('u',0xffffffff):(1048,-1,'C9',0xffffffff,0),('u',1):(1048,reg,sem,1,int(bool(sem)))},
            {reg:[2,3,4,5]},[2,-6,12,2.5])
    run('shadow-array','uniform float4 u[2]:C9; float4 main(float4 p:POSITION,uniform float4 u[2]:C20):POSITION{return p*u[1];}',
        {('u[0]',0xffffffff):(1048,-1,'C9',0xffffffff,0),('u[1]',0xffffffff):(1048,-1,'C9',0xffffffff,0),
         ('u[0]',1):(1048,-1,'C20',1,0),('u[1]',1):(1048,21,'C20',1,1)},
        {21:[2,3,4,5]},[2,-6,12,2.5])
    run('helper-shadow-array','uniform float4 u[2]:C9; float4 globalValue(){return u[1];} float4 main(float4 p:POSITION,uniform float4 u[2]:C20):POSITION{return p*u[1]+globalValue();}',
        {('u[0]',0xffffffff):(1048,-1,'C9',0xffffffff,0),('u[1]',0xffffffff):(1048,10,'C9',0xffffffff,1),
         ('u[0]',1):(1048,-1,'C20',1,0),('u[1]',1):(1048,21,'C20',1,1)},
        {21:[2,3,4,5],10:[7,11,13,17]},[9,5,25,19.5])
    run('helper-shadow-scalar','uniform float4 u:C9; float4 globalValue(){return u;} float4 main(float4 p:POSITION,uniform float4 u:C20):POSITION{return p*u+globalValue();}',
        {('u',0xffffffff):(1048,9,'C9',0xffffffff,1),('u',1):(1048,20,'C20',1,1)},
        {20:[2,3,4,5],9:[7,11,13,17]},[9,5,25,19.5])
    # Existing automatic struct allocation reserves its parent at c467;
    # the oracle omits that slot (a separate layout difference). Preserve
    # the leaf reads at our declared c466/c465 through owner propagation.
    run('struct-leaf-owner','struct U{float4 a;float4 b;}; uniform U u; float4 main(float4 p:POSITION):POSITION{return p*u.a+u.b;}',
        {'u.a':(1048,466,'',0xffffffff,0),'u.b':(1048,465,'',0xffffffff,0)},
        {466:[2,3,4,5],465:[7,11,13,17]},[9,5,25,19.5])
    for scope in ('global','entry'):
      for spelling in ('register(C9)','C9'):
        tag=scope+'-'+spelling
        declaration='uniform float4 u:'+spelling
        source=(declaration+'; float4 main(float4 p:POSITION):POSITION{return p*u;}') if scope=='global' else ('float4 main(float4 p:POSITION,'+declaration+'):POSITION{return p*u;}')
        run(tag,source,{'u':(1048,9,'C9',0xffffffff if scope=='global' else 1,1)},
            {9:[2,3,4,5]},[2,-6,12,2.5])
        if scope=='global':
            pin=spelling.startswith('register')
            run(tag+'-legacy',source,{'u':(1048,9 if pin else 467,'C9' if pin else '',0xffffffff,int(pin))},
                {9 if pin else 467:[2,3,4,5]},[2,-6,12,2.5],legacy=True)
    for width in (3,4):
      for scope in ('global','entry'):
        decl=f'uniform float{width}x{width} M:C9'
        expr='mul(M,p)' if width==4 else 'float4(mul(M,p.xyz),p.w)'
        source=(decl+'; float4 main(float4 p:POSITION):POSITION{return '+expr+';}') if scope=='global' else ('float4 main(float4 p:POSITION,'+decl+'):POSITION{return '+expr+';}')
        mat=matrix(0,width)
        run(f'matrix{width}-{scope}',source,rows('M',width,9,'C9',0xffffffff if scope=='global' else 1),
            {9+i:row+[0]*(4-width) for i,row in enumerate(mat)},product(mat,p)+([p[3]] if width==3 else []))
    for mode,selected in [('static',1),('dynamic',0),('dynamic',1)]:
        index='1' if mode=='static' else 'int(i)'
        expected={}
        for element in range(2):expected.update(rows(f'M[{element}]',4,9+element*4 if mode=='dynamic' or element==selected else -1,'C9'))
        constants={9+element*4+row:values for element in range(2) for row,values in enumerate(matrix(element,4))}
        run(f'array-{mode}-{selected}',f'uniform float4x4 M[2]:register(C9); float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION{{return mul(M[{index}],p);}}',
            expected,constants,product(matrix(selected,4),p),index=selected+0.75)
    run('unused-before-auto','uniform float4 u:C256; uniform float4 v; float4 main(float4 p:POSITION):POSITION{return p*v;}',
        {'u':(1048,-1,'C256',0xffffffff,0),'v':(1048,467,'',0xffffffff,0)}, {467:[2,3,4,5]},[2,-6,12,2.5])
    run('alias','uniform float4 u:C9; uniform float4 v:C9; float4 main(float4 p:POSITION):POSITION{return p*u+v;}',
        {'u':(1048,9,'C9',0xffffffff,1),'v':(1048,9,'C9',0xffffffff,1)}, {9:[2,3,4,5]},[4,-3,16,7.5])
    run('scalar255','uniform float4 u:register(C255); float4 main(float4 p:POSITION):POSITION{return p*u;}',
        {'u':(1048,255,'C255',0xffffffff,1)}, {255:[2,3,4,5]},[2,-6,12,2.5])
    run('scalar256','uniform float4 u:C256; float4 main(float4 p:POSITION):POSITION{return p*u;}',{},refusal=True)
    for base,selected,refusal in [(252,0,False),(252,1,True),(248,1,False)]:
        expected={}
        for element in range(2):expected.update(rows(f'M[{element}]',4,base+4*element if element==selected else -1,'C'+str(base)))
        run(f'sparse-boundary-{base}-{selected}',f'uniform float4x4 M[2]:C{base}; float4 main(float4 p:POSITION):POSITION{{return mul(M[{selected}],p);}}',
            expected,{base+4*selected+row:v for row,v in enumerate(matrix(selected,4))},product(matrix(selected,4),p),refusal=refusal)
    for base,refusal in [(252,False),(253,True)]:
        run('matrix-boundary-'+str(base),f'uniform float4x4 M:C{base}; float4 main(float4 p:POSITION):POSITION{{return mul(M,p);}}',
            rows('M',4,base,'C'+str(base)),{base+row:v for row,v in enumerate(matrix(0,4))},product(matrix(0,4),p),refusal=refusal)
    run('struct-entry','uniform float4 u:C9; uniform float4 v; struct Input{float4 p:POSITION;}; float4 main(Input i):POSITION{return i.p*u+v;}',
        {'u':(1048,9,'C9',0xffffffff,1),'v':(1048,467,'',0xffffffff,0)}, {9:[2,3,4,5],467:[1,2,3,4]},[3,-4,15,6.5])
    for spelling in ('register(C9)','C9'):
        expected={}
        for element in range(2):expected.update(rows(f'M[{element}]',4,9+4*element,'C9',2))
        run('entry-array-'+spelling,f'float4 main(float4 p:POSITION,float i:TEXCOORD0,uniform float4x4 M[2]:{spelling}):POSITION{{return mul(M[int(i)],p);}}',
            expected,{9+4*element+row:v for element in range(2) for row,v in enumerate(matrix(element,4))},
            product(matrix(1,4),p),index=1.75)
    for selected in (0,1):
        expected=rows('N',4,256,'')
        for element in range(2):expected.update(rows(f'M[{element}]',4,9+4*element,'C9'))
        expected.update({'V[0]':(1048,466,'',0xffffffff,0),'V[1]':(1048,467,'',0xffffffff,0)})
        constants={9+4*element+row:v for element in range(2) for row,v in enumerate(matrix(element,4))}
        constants.update({256+row:v for row,v in enumerate(matrix(2,4))})
        constants.update({466:[2,4,6,8],467:[3,5,7,9]})
        value=[a+b for a,b in zip(product(matrix(selected,4),product(matrix(2,4),p)),constants[466+selected])]
        run('mixed-stride-'+str(selected),'uniform float4x4 M[2]:C9; uniform float4x4 N; uniform float4 V[2]; float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION{return mul(M[int(i)],mul(N,p))+V[int(i)];}',
            expected,constants,value,index=selected+0.75)
    run('dynamic-overflow','uniform float4x4 M[2]:C249; float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION{return mul(M[int(i)],p);}',{},refusal=True)
if failures:
    for failure in failures:print('FAIL: '+failure,file=sys.stderr)
    sys.exit(1)
print('PASS: vp-explicit-binding (record fields, decoded reads, numeric values, legacy contract, use-sensitive bounds)')
PY

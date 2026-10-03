"""VP struct constructor assignment: snapshots and every semantic output.

Independently authored source/vector twins; expected values use scalar formulas.
Fixed/bool/narrow types, arrays, assignment values and helper copy-out retain
explicit refusals. Reference uint crashes are not classified as refusals.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from vp_pow_vector_check import evaluate
from uniform_container_check import check_container

INPUTS = [
    [[.25,-.5,1,1.5],[-1,.75,-.25,.5],[.5,.75,0,0],[-.25,-.5,0,0]],
    [[-1.5,1.25,.5,-.75],[.25,-1.25,1.5,-.5],[-1,.25,0,0],[.75,1.5,0,0]],
]

CASES = [{'name': 'initializer-control',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair s=Pair(p.xy,q.zw);return float4(s.a,s.b);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(p.xy,q.zw);}'},
 {'name': 'local-assignment',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair s;s=Pair(p.xy,q.zw);return float4(s.a,s.b);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(p.xy,q.zw);}'},
 {'name': 'snapshot-swap',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair s=Pair(p.xy,q.zw);s=Pair(s.b,s.a);return float4(s.a,s.b);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(q.zw,p.xy);}'},
 {'name': 'stale-component',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair s=Pair(p.xy,q.zw);s.a.x=q.x;s=Pair(p.zw,q.xy);return '
            'float4(s.a.x,s.a.y,s.b);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(p.zw,q.xy);}'},
 {'name': 'two-destinations',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair a=Pair(p.xy,p.zw);Pair '
            'b=Pair(q.xy,q.zw);a=Pair(b.b,b.a);b=Pair(a.b,a.a);return float4(a.a,b.a);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(q.zw,q.xy);}'},
 {'name': 'nested-constructor',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};struct Wrap{Pair child;float2 keep;};float4 main(float4 '
            'p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{Wrap w;w=Wrap(Pair(p.xy,q.zw),p.zw);return '
            'float4(w.child.a,w.child.b+w.keep);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(p.xy,q.zw+p.zw);}'},
 {'name': 'nested-destination',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};struct Wrap{Pair child;float2 keep;};float4 main(float4 '
            'p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{Wrap '
            'w=Wrap(Pair(p.xy,q.zw),p.zw);w.child=Pair(w.child.b,w.child.a);return '
            'float4(w.child.a,w.child.b+w.keep);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(q.zw,p.xy+p.zw);}'},
 {'name': 'nested-source-alias',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};struct Wrap{Pair child;float2 keep;};float4 main(float4 '
            'p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{Wrap '
            'w=Wrap(Pair(p.xy,q.zw),p.zw);w=Wrap(w.child,w.child.a);return float4(w.child.b,w.keep);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(q.zw,p.xy);}'},
 {'name': 'evaluation-once',
  'refusal': False,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{float x=p.x;Pair s;s=Pair(float2(x++,p.y),float2(x++,q.w));return '
            'float4(s.a.x,s.b.x,x,1);}',
  'twin': 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{return float4(p.x,p.x+1,p.x+2,1);}'},
 {'name': 'out-assignment',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,out Pair o){o=Pair(p.xy,q.zw);pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'out-member-store-control',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,out Pair o){o.a=p.xy;o.b=q.zw;pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'out-overwrite',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,out Pair '
            'o){o=Pair(q.xy,p.zw);o=Pair(p.xy,q.zw);pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'inout-assignment',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,inout Pair o){o=Pair(p.xy,q.zw);pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'inout-member-store-control',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,inout Pair o){o.a=p.xy;o.b=q.zw;pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'inout-overwrite',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,inout Pair '
            'o){o=Pair(q.xy,p.zw);o=Pair(p.xy,q.zw);pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'inout-read-before-write',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,inout Pair o){o=Pair(o.b,o.a);pos=float4(o.a,o.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1,float2 ia:TEXCOORD2,float2 ib:TEXCOORD3){Result '
          'r;r.a=ib;r.b=ia;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'nested-entry-output',
  'refusal': False,
  'source': 'struct Pair{float2 a:TEXCOORD2;float2 b:TEXCOORD3;};struct Wrap{Pair child;};void main(float4 '
            'p:TEXCOORD0,float4 q:TEXCOORD1,out float4 pos:POSITION,out Wrap '
            'o){o=Wrap(Pair(p.xy,q.zw));pos=float4(o.child.a,o.child.b);}',
  'twin': 'struct Result{float4 pos:POSITION;float2 a:TEXCOORD2;float2 b:TEXCOORD3;};Result main(float4 '
          'p:TEXCOORD0,float4 q:TEXCOORD1){Result r;r.a=p.xy;r.b=q.zw;r.pos=float4(r.a,r.b);return r;}'},
 {'name': 'kind-half',
  'refusal': True,
  'source': 'struct Typed{half2 a;half2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Typed s;s=Typed(half2(3,-3),half2(p.xy));return float4(s.a,s.b);}'},
 {'name': 'kind-int',
  'refusal': True,
  'source': 'struct Typed{int2 a;int2 b;};float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{Typed '
            's;s=Typed(int2(3,-3),int2(p.xy));return float4(s.a,s.b);}'},
 {'name': 'kind-fixed',
  'refusal': True,
  'source': 'struct Typed{fixed2 a;fixed2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Typed s;s=Typed(fixed2(3,-3),fixed2(p.xy));return float4(s.a,s.b);}'},
 {'name': 'kind-bool',
  'refusal': True,
  'source': 'struct Typed{bool2 a;bool2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Typed s;s=Typed(bool2(3,-3),bool2(p.xy));return float4(s.a,s.b);}'},
 {'name': 'kind-short',
  'refusal': True,
  'source': 'struct Typed{short2 a;short2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Typed s;s=Typed(short2(3,-3),short2(p.xy));return float4(s.a,s.b);}'},
 {'name': 'fixed-output-boundary',
  'refusal': True,
  'source': 'struct Typed{fixed2 a:TEXCOORD2;fixed2 b:TEXCOORD3;};void main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1,out float4 pos:POSITION,out Typed '
            'o){o=Typed(fixed2(3,-3),fixed2(p.xy));pos=float4(o.a,o.b);}'},
 {'name': 'array-field-boundary',
  'refusal': True,
  'source': 'struct ArrayPair{float a[2];float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{float v[2]={p.x,p.y};ArrayPair s;s=ArrayPair(v,q.xy);return '
            'float4(s.a[0],s.a[1],s.b);}'},
 {'name': 'indexed-destination-boundary',
  'refusal': True,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair a[2];a[0]=Pair(p.xy,q.zw);return float4(a[0].a,a[0].b);}'},
 {'name': 'assignment-value-boundary',
  'refusal': True,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair a;return float4((a=Pair(p.xy,q.zw)).a,q.xy);}'},
 {'name': 'chain-boundary',
  'refusal': True,
  'source': 'struct Pair{float2 a;float2 b;};float4 main(float4 p:TEXCOORD0,float4 '
            'q:TEXCOORD1):POSITION{Pair a;Pair b;a=(b=Pair(p.xy,q.zw));return float4(a.a,b.b);}'},
 {'name': 'helper-inout-boundary',
  'refusal': True,
  'source': 'struct Pair{float2 a;float2 b;};void fill(inout Pair s,float4 p,float4 '
            'q){s=Pair(p.xy,q.zw);}float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):POSITION{Pair '
            's=Pair(q.xy,p.zw);fill(s,p,q);return float4(s.a,s.b);}'}]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def expected(name, inputs):
    p,q,ia,ib=inputs
    normal=p[:2]+q[2:]
    values={
        'initializer-control':normal, 'local-assignment':normal,
        'snapshot-swap':q[2:]+p[:2],
        'stale-component':p[2:]+q[:2],
        'two-destinations':q[2:]+q[:2],
        'nested-constructor':p[:2]+[q[2+i]+p[2+i] for i in range(2)],
        'nested-destination':q[2:]+[p[i]+p[2+i] for i in range(2)],
        'nested-source-alias':q[2:]+p[:2],
        'evaluation-once':[p[0],p[0]+1,p[0]+2,1],
        'inout-read-before-write':ib[:2]+ia[:2],
    }
    position=values.get(name,normal)
    result={0:position}
    if name.startswith(('out-','inout-')) or name=='nested-entry-output':
        result.update({9:position[:2],10:position[2:]})
    return result


def check_reflection(blob, name):
    if not (name.startswith(('out-','inout-')) or name=='nested-entry-output'):
        return
    u=lambda off: struct.unpack_from('>I',blob,off)[0]
    records=[]
    for offset in range(u(16),u(16)+48*u(12),48):
        fields=struct.unpack_from('>12I',blob,offset)
        records.append((fields[1],fields[0],fields[8]))
    for resource in (3222,3223):  # declared TEXCOORD2/3, two float lanes each
        # The reference reflects inout fields as incoming ATTR10/11 records
        # even when the program writes TEXCOORD2/3. Numeric checks above must
        # still witness the physical outputs, independently of this metadata.
        reflected=(resource,1046,4098) in records or (name.startswith('inout-') and
                   (resource-1099,1046,4097) in records)
        require(reflected,name+': missing float2 semantic reflection '+str(resource))


def prepare(root):
    root.mkdir(parents=True,exist_ok=True)
    for case in CASES:
        for form in ('source',) if case['refusal'] else ('source','twin'):
            (root/(case['name']+'-'+form+'.cg')).write_text(case[form]+'\n')
    return CASES


def run(compiler, root):
    reports,failures=[],[]
    for case in prepare(root):
        for form in ('source',) if case['refusal'] else ('source','twin'):
            name=case['name']+'-'+form
            output=root/(name+'.bin')
            if output.exists(): output.unlink()
            try:
                result=subprocess.run([compiler,'-p','sce_vp_rsx','--emit-container',str(output),str(root/(name+'.cg'))],
                                      capture_output=True,text=True,timeout=20)
                (root/(name+'.log')).write_text(result.stdout+result.stderr)
                record=dict(name=name,status=result.returncode)
                if case['refusal']:
                    require(result.returncode==1 and not output.exists(),name+': expected refusal and no container')
                    record['refusal_verified']=True
                else:
                    require(result.returncode==0 and output.exists(),name+': compile failed: '+result.stderr)
                    blob=output.read_bytes()
                    require(not check_container(blob)['issues'],name+': container inconsistency')
                    values=[]
                    for inputs in INPUTS:
                        actual=evaluate(blob,{},inputs={8+i:v for i,v in enumerate(inputs)},binary32=True)
                        for register,want in expected(case['name'],inputs).items():
                            require(register in actual and actual[register][:len(want)]==want,
                                    name+': output '+str(register)+' got '+str(actual.get(register))+' expected '+str(want))
                        values.append(actual)
                    check_reflection(blob,case['name'])
                    record['values']=values
                reports.append(record)
            except (AssertionError,ValueError,KeyError,subprocess.TimeoutExpired) as error:
                failures.append(name+': '+str(error))
    (root/'RESULT.json').write_text(json.dumps(dict(reports=reports,failures=failures),indent=2)+'\n')
    require(not failures,'\n'.join(failures))
    print('PASS: struct assignment, complete snapshots, VP output values and reflection, bounded refusals')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler',nargs='?')
    parser.add_argument('--prepare-only',type=Path)
    parser.add_argument('--work-dir',type=Path)
    args=parser.parse_args()
    if args.prepare_only:
        prepare(args.prepare_only)
        print(str(len(CASES))+' cases prepared')
        return
    parser.error('compiler required') if not args.compiler else None
    if args.work_dir:
        run(args.compiler,args.work_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='struct-assignment-') as temp:
            run(args.compiler,Path(temp))


if __name__=='__main__':
    main()

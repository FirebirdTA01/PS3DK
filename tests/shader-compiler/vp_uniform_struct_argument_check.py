"""Uniform struct arguments must retain uniform provenance and private values."""
import argparse,hashlib,json,math,subprocess,tempfile
from pathlib import Path
from vp_pow_vector_check import evaluate
T='struct L { float4 a; float4 b; }; '
G='float4 g(L l,float4 v) { return l.a*v.x+l.b; } '
M='float4 main(float4 p:POSITION):POSITION { '
CASES={
 'plain':(T+'uniform L lt; '+G+M+'return g(lt,p); }','plain'),
 'literal':(T+'uniform L lt[3]; '+G+M+'return g(lt[1],p); }','literal'),
 'dynamic':(T+'uniform L lt[3]; '+G+M+'return g(lt[int(p.x)],p); }','dynamic'),
 'same_name':(T+'uniform L lt; '+G.replace('L l,','L lt,').replace('l.a','lt.a').replace('l.b','lt.b')+M+'return g(lt,p); }','plain'),
 'forward':(T+'uniform L lt; '+G+'float4 h(L l,float4 p){return g(l,p);} '+M+'return h(lt,p); }','plain'),
 'dynamic_forward':(T+'uniform L lt[3]; '+G+'float4 h(L l,float4 p){return g(l,p);} '+M+'return h(lt[int(p.x)],p); }','dynamic'),
 'local_copy':(T+'uniform L lt; '+G+M+'L q=lt; return g(q,p); }','plain'),
 'private_write':(T+'uniform L lt; float4 g(L l,float4 v){l.a=v;return l.a*v.x+l.b;} '+M+'return g(lt,p)+lt.a; }','private'),
 'argument_order':(T+'uniform L lt; '+G+M+'return g(lt,(lt.a=p)); }','order'),
 'two_dynamic':(T+'uniform L lt[3]; '+G+M+'return g(lt[int(p.x)],p)+g(lt[int(p.y)],p); }','two'),
 'index_once':(T+'uniform L lt[3]; '+G+M+'int i=1;float4 r=g(lt[i++],p);return r+float4(i,0,0,0); }','once'),
 'index_once_dynamic':(T+'uniform L lt[3]; '+G+M+'float i=p.x;float4 r=g(lt[i++],p);return r+float4(i,0,0,0); }','once_dynamic'),
 'index_order':(T+'uniform L lt[3]; '+G+M+'int i=0;return g(lt[i],float4(i=2,p.yzw)); }','index_order'),
 'index_order_arithmetic':(T+'uniform L lt[3]; '+G+M+'int i=0;return g(lt[i+0],float4(i=2,p.yzw)); }','index_order'),
 'index_order_builtin':(T+'uniform L lt[3]; '+G+M+'int i=0;return g(lt[int(abs(i))],float4(i=2,p.yzw)); }','index_order'),
}
REFUSALS={
 'index_unknown_call':(T+'uniform L lt[3]; '+G+'int pick(int i){return i;} '+M+'int i=0;return g(lt[pick(i)],float4(i=2,p.yzw)); }','struct-argument-index-effects'),
 'uncaptured_constructor':(T+G+M+'return g(L(p,p),p); }','struct-argument-binding'),
 'dynamic_nested':('struct I {float4 a,b;};struct L {I q;};uniform L lt[3];float4 g(L l,float4 v){return l.q.a*v.x+l.q.b;} '+M+'return g(lt[int(p.x)],p); }','struct-uniform-argument'),
 'sampler_member':('struct L {sampler2D s;float4 a;};uniform L lt;float4 g(L l,float4 v){return l.a*v.x;} '+M+'return g(lt,p); }','struct-uniform-argument'),
}

def agrees(a,b):
 return a is not None and len(a)==len(b)==4 and all(math.isfinite(x) and math.isfinite(y) and abs(x-y)<=1e-5*max(1.,abs(y)) for x,y in zip(a,b))

def main():
 ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--reference',action='store_true');ap.add_argument('--loops',action='store_true');ap.add_argument('--output',type=Path);a=ap.parse_args()
 td=None if a.output else tempfile.TemporaryDirectory(prefix='vp-uniform-struct-')
 root=a.output or Path(td.name)
 if a.output:root.mkdir(parents=True,exist_ok=False)
 cases=dict(CASES)
 if a.loops:cases['loop']=(T+'uniform L lt[3]; uniform float n; '+G+M+'float4 s=0;for(int i=0;i<n;++i)s+=g(lt[i],p);return s;}','loop')
 fails=[];results=[];th=hashlib.sha256(Path(a.compiler).read_bytes()).hexdigest()
 for name,(source,mode) in cases.items():
  src=root/(name+'.cg');out=root/(name+'.vpo');src.write_text(source+'\n')
  argv=[a.compiler,'-p','sce_vp_rsx','-e','main','-o' if a.reference else '--emit-container',str(out),str(src)]
  p=subprocess.run(argv,capture_output=True,timeout=60);log=p.stdout+p.stderr;(root/(name+'.log')).write_bytes(log)
  r={'case':name,'argv':argv,'rc':p.returncode,'tool_sha256':th,'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'log_sha256':hashlib.sha256(log).hexdigest()};results.append(r)
  if p.returncode or not out.exists() or not out.stat().st_size:
   fails.append(name+': compile not admitted');continue
  blob=out.read_bytes();r['output_sha256']=hashlib.sha256(blob).hexdigest();values=[]
  for seed in (0,1,3):
   va=[[.5+seed+i,1.25+i,-2.-i,.25+i] for i in range(3)]
   vb=[[-1.+i,.5+seed-i,2.25+i,1.-i] for i in range(3)]
   un={'lt.a':va[0],'lt.b':vb[0],'n':[float(seed),0,0,0]}
   for i in range(3):un.update({'lt[%d].a'%i:va[i],'lt[%d].b'%i:vb[i]})
   for v in ([0.,2.,-.5,1.],[1.,0.,1.5,-2.],[2.,1.,-1.,.25]):
    def f(i):return [x*v[0]+y for x,y in zip(va[i],vb[i])]
    want=f(1 if mode in ('literal','once') else int(v[0]) if mode in ('dynamic','two','once_dynamic') else 0)
    if mode=='two':want=[x+y for x,y in zip(want,f(int(v[1])))]
    if mode in ('private','order'):want=[x*v[0]+y for x,y in zip(v,vb[0])]
    if mode=='private':want=[x+y for x,y in zip(want,va[0])]
    if mode=='once':want[0]+=2
    if mode=='once_dynamic':want[0]+=v[0]+1
    if mode=='index_order':want=[x*2+y for x,y in zip(va[2],vb[2])]
    if mode=='loop':want=[sum(f(i)[c] for i in range(seed)) for c in range(4)]
    opts={'branches':True,'predication':True} if mode=='loop' else {'predication':True}
    try:got=evaluate(blob,un,inputs={0:v,1:[31.,32.,33.,34.],2:[41.,42.,43.,44.]},binary32=True,indexed_constants=True,**opts).get(0)
    except (AssertionError,ValueError,KeyError) as e:fails.append(name+': UNJUDGED '+str(e));continue
    ok=agrees(got,want);values.append({'input':v,'seed':seed,'got':got,'want':want,'ok':ok})
    if not ok:fails.append(name+': wrong value '+str((got,want)))
  r['values']=values
  if len(values)!=9:fails.append(name+': incomplete values')
  print(name,len(values),'checks',sum(x['ok'] for x in values),'correct',flush=True)
 if not a.reference:
  for name,(source,diagnostic) in REFUSALS.items():
   src=root/(name+'.cg');out=root/(name+'.vpo');src.write_text(source+'\n')
   argv=[a.compiler,'-p','sce_vp_rsx','-e','main','--emit-container',str(out),str(src)]
   p=subprocess.run(argv,capture_output=True,timeout=60);log=p.stdout+p.stderr;(root/(name+'.log')).write_bytes(log)
   ok=p.returncode==1 and not out.exists() and diagnostic.encode() in log
   results.append(dict(case=name,argv=argv,rc=p.returncode,tool_sha256=th,refused=ok,source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),log_sha256=hashlib.sha256(log).hexdigest()))
   if not ok:fails.append(name+': expected named refusal and no container')
 if agrees([math.nan]*4,[0.]*4) or agrees([],[0.]*4) or agrees([1.,0,0,0],[0.]*4):fails.append('judge negative controls')
 (root/'results.json').write_text(json.dumps(results,indent=2))
 for f in fails:print('FAIL:',f)
 print('vp-uniform-struct-argument:', 'FAIL' if fails else 'PASS')
 return bool(fails)
if __name__=='__main__':raise SystemExit(main())

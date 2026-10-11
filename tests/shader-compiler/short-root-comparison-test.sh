#!/usr/bin/env bash
# Reference lowering evaluates the root unconditionally, then compares it
# to a boolean before combining. A raw root value must never feed the
# boolean multiply. Runtime sign/zero behaviour is also judged on the rig.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$root" <<'PY'
import pathlib,re,subprocess,sys
compiler,root=sys.argv[1:]; errors=[]
sys.path.insert(0,str(pathlib.Path(root)/'tests/shader-compiler'))
# tree_not / tree_or: the right operand is a boolean tree over comparisons
# (!near(a,b), near(a,b) || d), evaluated eagerly as the reference does.
for tag in ('sqrt_and','sqrt_or','sqrt_zero','rsqrt_and','sqrt_discard','tree_not','tree_or'):
    p=pathlib.Path(root)/'tools/rsx-cg-compiler/tests/shaders'/('fp_short_'+tag+'_f.cg')
    r=subprocess.run([compiler,'-p','sce_fp_rsx','-e','main',str(p)],capture_output=True,text=True,timeout=30)
    if r.returncode:
        errors.append(tag+': did not compile: '+r.stderr.strip()); continue
    rows=[]
    for line in r.stdout.splitlines():
        m=re.fullmatch(r'\s*\d+:((?:\s+[0-9a-fA-F]{8}){4})\s*',line)
        if m: rows.append([((int(x,16)>>16)|(int(x,16)<<16))&0xffffffff for x in m[1].split()])
    insns=[]; i=0
    while i<len(rows):
        w=rows[i]; insns.append(w);i+=1+int(any((w[j]&3)==2 for j in (1,2,3)))
    if not insns: raise SystemExit('FAIL: empty instruction listing')
    op=0x1b if tag=='rsqrt_and' else 0x3b
    roots=[w for w in insns if ((w[0]>>24)&63)==op]
    if len(roots)!=1: errors.append(tag+': expected one reference root instruction')
    if tag!='rsqrt_and' and roots and not roots[0][1]&(1<<29):
        errors.append(tag+': DIVSQR numerator must carry abs modifier')
    combiner=3 if tag=='sqrt_or' else 2
    combines=[w for w in insns if ((w[0]>>24)&63)==combiner and w[0]&(1<<8)]
    if not combines: errors.append(tag+': missing CC-writing boolean combiner')
    if tag=='sqrt_or' and not any(w[0]&(1<<31) for w in combines):
        errors.append(tag+': boolean OR add must saturate')
# A shared boolean DAG (b1 = b0 && b0; b2 = b1 && b1; ... 40 deep, and the
# alternating &&/|| form) is decided once per node: the first tree walk
# revisited both edges of every shared node and did not finish in 60 s.
# Bounded here at 20 s; each must compile.
import tempfile
for alt in (False, True):
    lines=['float4 main(float4 t : TEXCOORD0) : COLOR {','  bool b0 = (t.x > 0.0);']
    for i in range(1,41):
        lines.append('  bool b%d = b%d %s b%d;'%(i,i-1,'||' if alt and i%2==0 else '&&',i-1))
    lines += ['  return (b40 && sqrt(t.y) < 0.1) ? float4(1,1,1,1) : float4(0,0,0,0);','}']
    src=pathlib.Path(tempfile.mkdtemp())/'dag.cg'; src.write_text(chr(10).join(lines)+chr(10))
    name='shared_dag_'+('alt' if alt else 'and')
    try:
        r=subprocess.run([compiler,'-p','sce_fp_rsx','-e','main',str(src)],capture_output=True,text=True,timeout=20)
        if r.returncode!=0: errors.append(name+': did not compile (exit %d)'%r.returncode)
    except subprocess.TimeoutExpired:
        errors.append(name+': did not finish within 20 s (exponential walk over a shared boolean DAG)')
p=pathlib.Path(root)/'tools/rsx-cg-compiler/tests/shaders/fp_short_sqrt_shared_refuse_f.cg'
r=subprocess.run([compiler,'-p','sce_fp_rsx',str(p)],capture_output=True,text=True,timeout=30)
# Exit 1 EXACTLY: a timeout or a crash also has a non-zero status, and both
# would have satisfied the old `not r.returncode` while meaning the compiler
# never reached this decision (crash-versus-refusal-status).
if r.returncode!=1:
    what=('died on signal %d'%-r.returncode) if r.returncode<0 else ('exited %d'%r.returncode)
    errors.append('shared root: expected exit 1, a refusal, but the compiler '+what+' - a timeout or a crash is not a refusal')
elif 'short-circuit' not in r.stderr:
    errors.append('shared root must keep its named short-circuit refusal')
if errors: raise SystemExit('\n'.join('FAIL: '+e for e in errors))
print('short-root-comparison: root-to-comparison shapes compile')
PY

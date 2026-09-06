#!/usr/bin/env bash
# Refract must compute sqrt(abs(k)) directly, with abs on BOTH operands.
# A reciprocal-of-reciprocal-root reintroduction must fail this byte guard.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$root" <<'PY'
import pathlib,re,subprocess,sys
compiler,root=sys.argv[1:]; errors=[]
for suffix in ('','_runtime','_tir','_zero'):
    name='fp_refract_root'+suffix+'_f'
    src=pathlib.Path(root)/'tools/rsx-cg-compiler/tests/shaders'/(name+'.cg')
    r=subprocess.run([compiler,'-p','sce_fp_rsx',str(src)],capture_output=True,text=True,timeout=30)
    if r.returncode: errors.append(name+': did not compile: '+r.stderr);continue
    words=[]
    for line in r.stdout.splitlines():
        m=re.fullmatch(r'\s*\d+:((?:\s+[0-9a-fA-F]{8}){4})\s*',line)
        if m:words.append([((int(x,16)>>16)|(int(x,16)<<16))&0xffffffff for x in m[1].split()])
    if not words:raise SystemExit('FAIL: no emitted instruction words')
    insns=[];i=0
    while i<len(words):
        w=words[i];insns.append(w);i+=1+int(any((w[j]&3)==2 for j in (1,2,3)))
    roots=[w for w in insns if ((w[0]>>24)&63)==0x3b]
    if len(roots)!=1:errors.append(f'{name}: expected one DIVSQR, got {len(roots)}')
    if any(((w[0]>>24)&63) in (0x1a,0x1b) for w in insns):
        errors.append(name+': refract still uses RCP/RSQ approximation composition')
    for w in roots:
        if not w[1]&(1<<29) or not w[2]&(1<<18):
            errors.append(name+': root requires abs on both numerator and denominator')
        if (w[1]&0x1ffff)!=(w[2]&0x1ffff):
            errors.append(name+': root operands select different values or lanes')
        if (w[1]|w[2])&(1<<17):errors.append(name+': absolute root must not retain negation')
if errors:raise SystemExit('\n'.join('FAIL: '+e for e in errors))
print('refract-root: one native absolute root; runtime eta, TIR and zero shapes covered')
PY

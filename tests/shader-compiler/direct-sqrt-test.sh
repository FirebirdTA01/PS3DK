#!/usr/bin/env bash
# One native DIVSQR(abs(x),x) per root component: composing RSQ and RCP
# changes low bits. The residue fixture exposes that difference on the rig.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$root" <<'PY'
import pathlib,re,subprocess,sys
compiler,root=sys.argv[1:]; errors=[]
for name,count in [('direct',2),('swizzle',4),('residue',1),('zero_value',1),('negative_value',1)]:
    src=pathlib.Path(root)/'tools/rsx-cg-compiler/tests/shaders'/('fp_sqrt_'+name+'_f.cg')
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
    if len(roots)!=count: errors.append(f'{name}: expected {count} DIVSQR, got {len(roots)}')
    if any(((w[0]>>24)&63) in (0x1a,0x1b) for w in insns):
        errors.append(name+': root still uses RCP/RSQ approximation composition')
    for w in roots:
        if not w[1]&(1<<29): errors.append(name+': missing numerator abs modifier')
        # Low source bits encode register, precision and swizzle. Negation
        # may differ because the numerator takes abs of a negated view.
        # Numerator and denominator must address the same component.
        if (w[1]&0x1ffff)!=(w[2]&0x1ffff):
            errors.append(name+': numerator and denominator select different values')
    if name=='swizzle':
        masks=[(w[0]>>9)&15 for w in roots]
        if sorted(masks)!=[1,2,4,8]: errors.append('swizzle: roots must cover each destination lane once')
if errors:raise SystemExit('\n'.join('FAIL: '+e for e in errors))
print('direct-sqrt: scalar, swizzled vector and residue use one native root per component')
PY

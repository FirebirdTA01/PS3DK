#!/usr/bin/env bash
# faceforward defaults to -N and selects N only when dot(I,Ng) is LT zero.
# Pin the emitted CC predicate, its x swizzle, and the operand-width dot.
# Inline constant blocks are data and must not count as instructions.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$repo_root" <<'PY'
import pathlib, re, subprocess, sys
compiler, root = sys.argv[1:]
shaders = pathlib.Path(root) / 'tools/rsx-cg-compiler/tests/shaders'
errors = []
for tag, dot_op in [('1',0x02),('2',0x38),('3',0x05),('4',0x06),('_zero',0x06),('_lane',0x05)]:
    source = shaders / ('fp_faceforward%s_f.cg' % tag)
    run = subprocess.run([compiler,'-p','sce_fp_rsx',str(source)],
                         capture_output=True,text=True,timeout=30)
    if run.returncode:
        errors.append('%s did not compile: %s' % (source.name,(run.stdout+run.stderr).strip()))
        continue
    words=[]
    for line in run.stdout.splitlines():
        match=re.fullmatch(r'\s*\d+:((?:\s+[0-9a-fA-F]{8})+)\s*',line)
        if match:
            words.append([((int(x,16)>>16)|(int(x,16)<<16))&0xffffffff for x in match[1].split()])
    insns=[]; i=0
    while i<len(words):
        w=words[i]
        if len(w)!=4: raise SystemExit('FAIL: malformed instruction listing')
        insns.append(w)
        i+=1+int(any((w[s]&3)==2 for s in (1,2,3)))
    if not insns: raise SystemExit('FAIL: empty ucode listing for '+source.name)
    dots=[i for i,w in enumerate(insns) if ((w[0]>>24)&0x3f) in (0x02,0x38,0x05,0x06) and (w[0]&(1<<8))]
    if len(dots)!=1 or ((insns[dots[0]][0]>>24)&0x3f)!=dot_op:
        errors.append(source.name+': wrong dot width/count'); continue
    d=dots[0]
    if not (insns[d][0]&(1<<8)) or not (insns[d][0]&(1<<30)):
        errors.append(source.name+': dot must write condition code only')
    commits=[i for i,w in enumerate(insns) if ((w[0]>>24)&0x3f)==1 and ((w[1]>>18)&7)==1]
    if len(commits)!=1:
        errors.append(source.name+': missing unique strict-LT conditional MOV'); continue
    c=commits[0]; pred=insns[c]
    if c<=d or ((pred[1]>>21)&255)!=0:
        errors.append(source.name+': conditional MOV must read CC.x after the dot')
    # Negated default and conditional commit must write the same physical
    # destination, including precision and mask, before any output copy.
    dst=pred[0]&0x1efe
    defaults=[w for w in insns[:c] if ((w[0]>>24)&0x3f)==1 and
              (w[0]&0x1efe)==dst and ((w[1]>>18)&7)==7 and (w[1]&(1<<17))]
    if not defaults: errors.append(source.name+': no negated default before conditional commit')
if errors: raise SystemExit('\n'.join('FAIL: '+error for error in errors))
print('faceforward: width-specific dot, negative default, strict LT on CC.x')
PY

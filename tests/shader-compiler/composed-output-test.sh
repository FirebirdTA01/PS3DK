#!/usr/bin/env bash
# Execute the MOV/ADD subset used by these minimal fixtures from emitted
# bytes. Distinct lane values expose a broadcast or lost preserved lane;
# merely unioning destination masks would pass both original defects.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$root" <<'PY'
import pathlib, re, struct, subprocess, sys
compiler, root = sys.argv[1:]
errors=[]
def listing(name, profile):
    p=pathlib.Path(root)/'tools/rsx-cg-compiler/tests/shaders'/name
    r=subprocess.run([compiler,'-p',profile,str(p)],capture_output=True,text=True,timeout=30)
    if r.returncode: raise SystemExit('FAIL: '+name+' did not compile: '+r.stderr)
    rows=[]
    for line in r.stdout.splitlines():
        m=re.fullmatch(r'\s*\d+:((?:\s+[0-9a-fA-F]{8}){4})\s*',line)
        if m: rows.append([int(x,16) for x in m[1].split()])
    if not rows: raise SystemExit('FAIL: empty instruction listing: '+name)
    return rows

color=[.125,.25,.375,.5]; offset=[.0625,.125,.1875,.25]
for name in ['vp_insert_alpha_last_v.cg','vp_insert_alpha_first_v.cg']:
    regs={}; outputs={}
    for w in listing(name,'sce_vp_rsx'):
        op=(w[1]>>22)&31
        if op not in (1,3): raise SystemExit('FAIL: unexpected VP opcode '+str(op))
        encoded=[((w[1]&255)<<9)|(w[2]>>23),
                 (w[2]>>6)&0x1ffff,((w[2]&63)<<11)|(w[3]>>21)]
        def source(s):
            kind=s&3; idx=(s>>2)&31
            if kind==1: v=regs.get(idx,[float('nan')]*4)
            elif kind==2: v=color if ((w[1]>>8)&15)==3 else [0,0,0,1]
            elif kind==3: v=offset
            else: raise SystemExit('FAIL: unsupported VP source')
            return [(-1 if s&(1<<16) else 1)*v[(s>>(14-2*k))&3] for k in range(4)]
        v=source(encoded[0])
        if op==3: v=[a+b for a,b in zip(v,source(encoded[2]))]
        out=bool(w[0]&(1<<30)); idx=((w[3]>>2)&31) if out else ((w[0]>>15)&63)
        dest=(outputs if out else regs).setdefault(idx,[float('nan')]*4)
        mask=(w[3]>>13)&15
        for k in range(4):
            if mask&(8>>k): dest[k]=v[k]
    want=[a+b for a,b in zip(color,offset)]
    if outputs.get(1)!=want: errors.append(name+': COLOR lanes '+str(outputs.get(1))+' != '+str(want))

name='fp_insert_constant_base_f.cg'
rows=listing(name,'sce_fp_rsx'); regs={}; i=0
while i<len(rows):
    raw=rows[i]; w=[((x>>16)|(x<<16))&0xffffffff for x in raw]; i+=1
    op=(w[0]>>24)&63
    if op!=1: raise SystemExit('FAIL: unexpected FP opcode '+str(op))
    const=None
    if any((w[s]&3)==2 for s in (1,2,3)):
        if i==len(rows): raise SystemExit('FAIL: missing constant block')
        const=[struct.unpack('!f',struct.pack('!I',((x>>16)|(x<<16))&0xffffffff))[0] for x in rows[i]]
        i+=1
    src=w[1]; kind=src&3
    v=regs.get((src>>2)&63,[float('nan')]*4) if kind==0 else color if kind==1 else const
    if v is None: raise SystemExit('FAIL: missing FP source')
    value=[v[(src>>(9+2*k))&3] for k in range(4)]
    dest=regs.setdefault((w[0]>>1)&63,[float('nan')]*4)
    for k in range(4):
        if w[0]&(1<<(9+k)): dest[k]=value[k]
want=[.25,color[1],.75,1.0]
if regs.get(0)!=want: errors.append(name+': COLOR lanes '+str(regs.get(0))+' != '+str(want))
# clamp writes its result repeatedly and its final MAX writes every lane.
# This is a real multiwriter (unlike successive SSA assignments). On the
# unmodified 2f42adf binary it emits six instruction slots. Preserve that
# full-mask fold instead of adding an export to every composed value.
full=listing('fp_composed_full_mask_f.cg','sce_fp_rsx')
if len(full)!=6: errors.append('full-mask multiwriter changed from parent count 6 to '+str(len(full)))
if errors: raise SystemExit('\n'.join('FAIL: '+e for e in errors))
print('composed-output: alpha-first/last and constant-base lanes preserved')
PY

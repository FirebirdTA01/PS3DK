#!/usr/bin/env bash
# t_ef0cb2e0, layout/load slice. Reference PS3_475 measurements are in the
# local build/array-probe/results.json: one0/one1/both/dynamic/param/mat3/
# mixed/stride_mix. Matrix elements grow from c256, unlike vector arrays.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" "$repo_root/tests/shader-compiler" <<'PY'
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0, sys.argv[2])
from vp_words import decode

compiler = str(Path(sys.argv[1]).resolve())
failures = []
def require(ok, message):
    if not ok: failures.append(message)

def container(path):
    b = path.read_bytes()
    h = struct.unpack_from('>8I', b)
    records = {}
    for i in range(h[3]):
        r = struct.unpack_from('>12I', b, h[4]+48*i)
        name = b[r[4]:b.index(0, r[4])].decode()
        require(name not in records, 'duplicate parameter '+name)
        records[name] = r
    words, error = decode(b)
    require(error is None, str(error))
    return b, records, words

# Deliberately asymmetric rows/elements; this oracle for values does not
# derive an expected address from the compiler's own register selection.
def matrix(element, width):
    return [[(element+1)*10 + row*3 + col/4 for col in range(width)]
            for row in range(width)]
def product(m, p):
    return [sum(a*b for a,b in zip(row,p)) for row in m]

def execute(b, records, words, index):
    constants = {}
    for name, r in records.items():
        if name.startswith('internal-constant-'):
            constants[r[3]] = list(struct.unpack_from('>4f', b, r[5]))
        match = re.fullmatch(r'M\[(\d+)\]\[(\d+)\]', name)
        if match and r[10]:
            elem,row = map(int, match.groups())
            width = r[0]-1044
            constants[r[3]] = matrix(elem,width)[row]+[0]*(4-width)
        if name == 'V[0]': constants[r[3]] = [2,4,6,8]
        if name == 'V[1]': constants[r[3]] = [3,5,7,9]
    regs = {'IN0':[1,-2,3,0.5], 'IN8':[index]*4, 'A0':[0]*4, 'A1':[0]*4}
    trace = []
    def source(text):
        neg = text.startswith('-'); text = text.lstrip('-')
        base,swizzle = text.rsplit('.',1)
        if base.startswith('C['):
            match = re.fullmatch(r'C\[(A[01])\.([xyzw])\+(\d+)\]',base)
            addr,lane,offset = match.groups()
            slot = int(offset)+int(regs[addr]['xyzw'.index(lane)])
            trace.append(slot)
            value = constants[slot]
        elif base.startswith('C'): value = constants[int(base[1:])]
        else: value = regs[base]
        return [(-1 if neg else 1)*value['xyzw'.index(l)] for l in swizzle]
    for line in words:
        match = re.fullmatch(r'\d+ (\w+) dst=(\w+) mask=([xyzw-]+) src0=(\S+) src1=(\S+) src2=(\S+)',line)
        op,dst,mask,*sources = match.groups()
        if op == 'NOP': continue
        a = source(sources[0])
        if op in ('MOV','ARL','FLR'):
            value = [math.floor(v) for v in a] if op != 'MOV' else a
        else:
            # VP ADD encodes its second operand in src2, unlike MUL/DP.
            c = source(sources[2] if op == 'ADD' else sources[1])
            if op == 'MUL': value = [x*y for x,y in zip(a,c)]
            elif op == 'ADD': value = [x+y for x,y in zip(a,c)]
            elif op == 'MAD': value = [x*y+z for x,y,z in zip(a,c,source(sources[2]))]
            elif op in ('DP3','DP4'): value = [sum(x*y for x,y in zip(a[:int(op[-1])],c))]*4
            else: raise ValueError('unsupported instruction in numeric witness: '+line)
        regs.setdefault(dst,[0]*4)
        for lane in mask:
            if lane != '-': regs[dst]['xyzw'.index(lane)] = value['xyzw'.index(lane)]
    return regs['o0'],trace

with tempfile.TemporaryDirectory(prefix='ps3dk-vp-matrix-array-') as temp:
    root = Path(temp)
    def compile_case(tag, source, refusal=None):
        src,out = root/(tag+'.cg'),root/(tag+'.bin')
        src.write_text(source)
        run = subprocess.run([compiler,'-p','sce_vp_rsx','--emit-container',str(out),str(src)],
                             capture_output=True,text=True,timeout=20)
        if refusal:
            require(run.returncode == 1 and refusal in run.stderr,
                    f'{tag}: expected exit 1 naming {refusal!r}, got {run.returncode}: {run.stderr}')
            return None
        if run.returncode:
            failures.append(f'{tag}: compiler exit {run.returncode}: {run.stderr.strip()}')
            return None
        return container(out)

    def check_records(tag, data, width, bases, parameter=False):
        _,records,_ = data
        # one0/one1: all unreferenced parent AND row records are declared,
        # resource=3256, reference=0, index=FFFFFFFF; dynamic: all referenced.
        names = [f'M[{e}]'+suffix for e in range(2)
                 for suffix in ['']+[f'[{r}]' for r in range(width)]]
        require([n for n in records if n.startswith('M')] == names,
                tag+': element/row order or whole-array parent')
        for e,base in enumerate(bases):
            for row in range(-1,width):
                name = f'M[{e}]'+(f'[{row}]' if row >= 0 else '')
                if name not in records:
                    failures.append(tag+': missing '+name); continue
                r = records[name]
                typ = (1059 if width == 3 else 1064) if row < 0 else 1044+width
                expected = (typ,2178 if base >= 0 else 3256,4102,
                            base+max(row,0) if base >= 0 else 0xffffffff,
                            4097,2 if parameter else 0xffffffff,int(base >= 0),0)
                got = (r[0],r[1],r[2],r[3],r[8],r[9],r[10],r[11])
                require(got == expected,f'{tag}: {name} fields {got}, expected {expected}')
                require(r[5] == 0 and r[6] == 0,tag+': unexpected default/embedded constants')

    for width in (3,4):
        p = 'p.xyz' if width == 3 else 'p'
        result = f'float4(mul(M[INDEX],{p}),1)' if width == 3 else 'mul(M[INDEX],p)'
        for tag,index,bases in [('one0','0',[256,-1]),('one1','1',[-1,256]),
                                ('dynamic','int(i)',[256,256+width])]:
            label = f'{tag}-{width}'
            body = result.replace('INDEX',index)
            data = compile_case(label,f'uniform float{width}x{width} M[2]; '
                     f'float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION {{return {body};}}')
            if data:
                check_records(label,data,width,bases)
                for value in (0.75,1.75):
                    try:
                        got,_ = execute(*data,value)
                        elem = int(value) if tag == 'dynamic' else int(index)
                        expected = product(matrix(elem,width),[1,-2,3,0.5][:width])
                        if width == 3: expected.append(1)
                        require(got == expected,f'{label}: at i={value}, {got} != {expected}')
                    except (KeyError,ValueError) as error: failures.append(label+': '+str(error))

    data = compile_case('parameter','float4 main(float4 p:POSITION,float i:TEXCOORD0,'
                        'uniform float4x4 M[2]):POSITION {return mul(M[int(i)],p);}')
    if data: check_records('parameter',data,4,[256,260],True)
    for tag,expr,bases in [('both-static','mul(M[0],p)+mul(M[1],p)',[256,260]),
                           ('folded','mul(M[int(1.0)],p)',[-1,256])]:
        data = compile_case(tag,'uniform float4x4 M[2]; float4 main(float4 p:POSITION):POSITION '
                            '{return '+expr+';}')
        if data:
            check_records(tag,data,4,bases)
            try:
                got,_ = execute(*data,0.75)
                expected = product(matrix(1,4),[1,-2,3,0.5])
                if tag == 'both-static':
                    expected = [a+b for a,b in zip(expected,product(matrix(0,4),[1,-2,3,0.5]))]
                require(got == expected,f'{tag}: {got} != {expected}')
            except (KeyError,ValueError) as error: failures.append(tag+': '+str(error))
    data = compile_case('mixed','uniform float4x4 N; uniform float4x4 M[2]; uniform float4 scale; '
                        'float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION '
                        '{return mul(N,mul(M[int(i)],p))*scale;}')
    if data:
        check_records('mixed',data,4,[260,264])
        require(data[1]['N'][3] == 256 and data[1]['scale'][3] == 467,'mixed: ordinary uniform cursors')
    data = compile_case('stride-mix','uniform float4x4 M[2]; uniform float4 V[2]; '
                        'float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION '
                        '{return mul(M[int(i)],p)+V[int(i)];}')
    if data:
        check_records('stride-mix',data,4,[256,260])
        words = data[2]
        ops = [line.split()[1] for line in words]
        require('FLR' in ops and 'MUL' in ops and ops.count('ARL') == 2,
                'stride-mix: must floor, scale and use distinct address demands: '+str(words))
        if all(op in ops for op in ('FLR','MUL','ARL')):
            require(ops.index('FLR') < ops.index('MUL') < ops.index('ARL'),
                    'stride-mix: floor must precede row-stride multiplication and ARL')
        for value in (0.75,1.75):
            try:
                got,reads = execute(*data,value)
                expected = [a+b for a,b in zip(product(matrix(int(value),4),[1,-2,3,0.5]),
                                               [2,4,6,8] if value < 1 else [3,5,7,9])]
                require(got == expected,f'stride-mix i={value}: {got} != {expected}')
                require(set(reads) == set(range(256+4*int(value),260+4*int(value)))|{466+int(value)},
                        f'stride-mix i={value}: wrong decoded constant reads {reads}')
            except (KeyError,ValueError) as error: failures.append('stride-mix: '+str(error))
    compile_case('arithmetic','uniform float4x4 M[2]; float4 main(float4 p:POSITION):POSITION '
                 '{return mul(0.5*M[0]+M[1],p);}', 'matrix arithmetic is not yet lowered')
    compile_case('binding','uniform float4x4 M[2]:register(C9); '
                 'float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION '
                 '{return mul(M[int(i)],p);}')
    compile_case('bound-matrix-beside-array','uniform float4x4 N:register(C9); '
                 'uniform float4x4 M[2]; float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION '
                 '{return mul(N,mul(M[int(i)],p));}')
    compile_case('bound-vector-beside-array','uniform float4 scale:register(C9); '
                 'uniform float4x4 M[2]; float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION '
                 '{return scale*mul(M[int(i)],p);}')
    compile_case('bound-entry-beside-array','uniform float4x4 M[2]; '
                 'float4 main(float4 p:POSITION,float i:TEXCOORD0,'
                 'uniform float4 scale:register(C9)):POSITION '
                 '{return scale*mul(M[int(i)],p);}')

if failures:
    for failure in failures: print('FAIL: '+failure,file=sys.stderr)
    sys.exit(1)
print('PASS: vp-matrix-array (records, row values, fractional indices, distinct strides, named refusals)')
PY

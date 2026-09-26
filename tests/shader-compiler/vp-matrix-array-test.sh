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

def execute(b, records, words, index, address_only=False):
    constants = {}
    for name, r in records.items():
        if name.startswith('internal-constant-'):
            constants[r[3]] = list(struct.unpack_from('>4f', b, r[5]))
        match = re.fullmatch(r'([MN])\[(\d+)\]\[(\d+)\]', name)
        if match and r[10]:
            array,elem,row = match.groups()
            elem,row = int(elem),int(row)
            if array == 'N': elem += 10
            width = r[0]-1044
            constants[r[3]] = matrix(elem,width)[row]+[0]*(4-width)
        if name == 'V[0]': constants[r[3]] = [2,4,6,8]
        if name == 'V[1]': constants[r[3]] = [3,5,7,9]
    indices = list(index) if isinstance(index, (list,tuple)) else [index]*4
    regs = {'IN0':[1,-2,3,0.5], 'IN8':indices, 'A0':[math.nan]*4, 'A1':[math.nan]*4}
    trace = []
    def source(text):
        neg = text.startswith('-'); text = text.lstrip('-')
        base,swizzle = text.rsplit('.',1)
        if base.startswith('C['):
            match = re.fullmatch(r'C\[(A[01])\.([xyzw])\+(\d+)\]',base)
            addr,lane,offset = match.groups()
            slot = int(offset)+int(regs[addr]['xyzw'.index(lane)])
            trace.append(slot)
            # For negative-index probes only, observe the address arithmetic
            # without claiming a defined shader value outside the array.
            value = constants.get(slot,[0]*4) if address_only else constants[slot]
        elif base.startswith('C'): value = constants[int(base[1:])]
        else: value = regs[base]
        return [(-1 if neg else 1)*value['xyzw'.index(l)] for l in swizzle]
    ucode_offset = struct.unpack_from('>I',b,28)[0]
    for number,line in enumerate(words):
        hw = struct.unpack_from('>4I',b,ucode_offset+16*number)
        if hw[1] >> 27 or hw[0] & ((7 << 21) | (1 << 13)):
            raise ValueError('numeric witness cannot model scalar ops, absolute sources or predication')
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
        regs.setdefault(dst,[math.nan]*4)
        for lane in mask:
            if lane != '-': regs[dst]['xyzw'.index(lane)] = value['xyzw'.index(lane)]
    return regs['o0'],trace

with tempfile.TemporaryDirectory(prefix='ps3dk-vp-matrix-array-') as temp:
    root = Path(temp)
    def compile_case(tag, source, refusal=None, optional_refusal=None):
        src,out = root/(tag+'.cg'),root/(tag+'.bin')
        src.write_text(source)
        run = subprocess.run([compiler,'-p','sce_vp_rsx','--emit-container',str(out),str(src)],
                             capture_output=True,text=True,timeout=20)
        if optional_refusal and run.returncode == 1 and optional_refusal in run.stderr:
            require(not out.exists(),f'{tag}: refusal left an output')
            return None
        if refusal:
            require(run.returncode == 1 and refusal in run.stderr,
                    f'{tag}: expected exit 1 naming {refusal!r}, got {run.returncode}: {run.stderr}')
            return None
        if run.returncode:
            failures.append(f'{tag}: compiler exit {run.returncode}: {run.stderr.strip()}')
            return None
        if not out.is_file() or out.stat().st_size < 32:
            failures.append(f'{tag}: compiler accepted without a container')
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

    # Distinct fractional lane inputs detect selecting x for every index,
    # swapping first-use order, or scaling by columns instead of rows.
    # Scalar casts are the already-supported controls for vector casts.
    for rows in (3,4):
        for label,declaration,left,right,lanes in (
            ('scalar-control','', 'int(i.y)','int(i.x)',(1,0)),
            ('vector-lanes','int4 ix=int4(i);','ix.y','ix.x',(1,0)),
            ('swizzled-lanes','int4 ix=int4(i.wzyx);','ix.y','ix.x',(2,3)),
            ('subscript-lanes','int4 ix=int4(i);','ix[1]','ix[0]',(1,0)),
        ):
            tag = f'{label}-{rows}x4'
            expression = f'mul(M[{left}],p)+2*mul(M[{right}],p)'
            if rows == 3: expression = f'float4({expression},1)'
            data = compile_case(tag,f'uniform float{rows}x4 M[8]; '
                    'float4 main(float4 p:POSITION,float4 i:TEXCOORD0):POSITION {'
                    +declaration+'return '+expression+';}')
            if data:
                for indices in ([1.75,2.25,4.125,6.875],[3.125,0.875,5.25,7.25]):
                    try:
                        got,reads = execute(*data,indices)
                        a,b = [math.floor(indices[lane]) for lane in lanes]
                        va = product(matrix(a,4)[:rows],[1,-2,3,0.5])
                        vb = product(matrix(b,4)[:rows],[1,-2,3,0.5])
                        expected = [x+2*y for x,y in zip(va,vb)]
                        swapped = [y+2*x for x,y in zip(va,vb)]
                        if rows == 3:
                            expected.append(1)
                            swapped.append(1)
                        require(got == expected,f'{tag} i={indices}: {got} != {expected}')
                        require(got != swapped,f'{tag}: swapped-lane control did not discriminate')
                        expected_reads = {256+rows*e+r for e in (a,b) for r in range(rows)}
                        require(set(reads) == expected_reads,
                                f'{tag} i={indices}: decoded reads {reads} != {sorted(expected_reads)}')
                    except (KeyError,ValueError) as error: failures.append(tag+': '+str(error))
                # The reference FLR/stride/ARL chain floors -0.5 to -1.
                # Judge decoded addresses only: out-of-array source-level
                # results have no defined value to compare here.
                indices = [-0.5,1.75,-0.5,1.75]
                try:
                    _,reads = execute(*data,indices,address_only=True)
                    elements = [math.floor(indices[lane]) for lane in lanes]
                    expected_reads = {256+rows*e+r for e in elements for r in range(rows)}
                    require(set(reads) == expected_reads,
                            f'{tag}: negative fraction addresses {reads} != {sorted(expected_reads)}')
                except (KeyError,ValueError) as error: failures.append(tag+': '+str(error))

    # A vector made from cast lanes and independent integer constants must
    # never be aliased wholesale to i. Either preserve lane provenance or
    # retain the existing named refusal for the unsupported value cast.
    data = compile_case('mixed-origin-lanes','uniform float4x4 M[8]; '
            'float4 main(float4 p:POSITION,float4 i:TEXCOORD0):POSITION {'
            'int4 cast=int4(i); int4 ix=int4(cast.xy,0,1); '
            'return mul(M[ix.x],p)+2*mul(M[ix.z],p)+3*mul(M[ix.w],p);}',
            optional_refusal='VP float-to-int lowering deferred')
    if data:
        for indices in ([2.75,3.25,4.5,5.5],[6.125,7.25,3.5,2.5]):
            try:
                got,_ = execute(*data,indices)
                parts = [product(matrix(e,4),[1,-2,3,0.5])
                         for e in (math.floor(indices[0]),0,1)]
                expected = [a+2*b+3*c for a,b,c in zip(*parts)]
                require(got == expected,f'mixed-origin-lanes: {got} != {expected}')
            except (KeyError,ValueError) as error: failures.append('mixed-origin-lanes: '+str(error))

    # Four lanes, each used with two distinct matrix row strides, occupy
    # all eight address lanes. A repeated lane must reuse its demand.
    terms = [f'{k+1}*(mul(M[ix.{lane}],p)+float4(mul(N[ix.{lane}],p),0))'
             for k,lane in enumerate('xyzw')]
    data = compile_case('two-arrays-eight-demands',
            'uniform float4x4 M[8]; uniform float3x4 N[8]; '
            'float4 main(float4 p:POSITION,float4 i:TEXCOORD0):POSITION {'
            'int4 ix=int4(i); return '+'+'.join(terms)+'+mul(M[ix[0]],p.wzyx);}')
    if data:
        for indices in ([0.75,1.5,2.25,3.5],[7.25,5.5,3.75,1.125]):
            try:
                got,reads = execute(*data,indices)
                elements = [math.floor(v) for v in indices]
                expected = product(matrix(elements[0],4),[0.5,3,-2,1])
                for k,e in enumerate(elements):
                    a = product(matrix(e,4),[1,-2,3,0.5])
                    b = product(matrix(e+10,4)[:3],[1,-2,3,0.5])+[0]
                    expected = [x+(k+1)*(y+z) for x,y,z in zip(expected,a,b)]
                require(got == expected,f'two-arrays-eight-demands: {got} != {expected}')
                expected_reads = {256+4*e+r for e in elements for r in range(4)}
                expected_reads |= {288+3*e+r for e in elements for r in range(3)}
                require(set(reads) == expected_reads,
                        f'two-arrays-eight-demands: wrong constant reads {reads}')
            except (KeyError,ValueError) as error: failures.append('two-arrays-eight-demands: '+str(error))

    # Index-only aliases must not leak into arithmetic or output values.
    compile_case('mixed-value-use','uniform float4x4 M[8]; '
            'float4 main(float4 p:POSITION,float4 i:TEXCOORD0):POSITION {'
            'int4 ix=int4(i); return mul(M[ix.y],p)+float4(ix);}',
            refusal='VP float-to-int lowering deferred')
    compile_case('mixed-descendant-use','uniform float4x4 M[8]; '
            'float4 main(float4 p:POSITION,float4 i:TEXCOORD0):POSITION {'
            'int4 ix=int4(i); int2 iy=ix.yx; return mul(M[iy.x],p)+float(iy.y);}',
            refusal='VP float-to-int lowering deferred')

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
                 '{return mul(0.5*M[0]+M[1],p);}')
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

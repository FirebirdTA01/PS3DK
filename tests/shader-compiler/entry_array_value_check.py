"""Entry input arrays: reference-measured element bindings, not vector lanes.

The reference reads TEXCOORD0 and TEXCOORD1 for float4 a[2]:TEXCOORD0.
The pre-array compiler instead broadcast TEX0.x/TEX0.y, and member-array
tracking then hid that fallback on helper calls. Distinct inputs prove values
independently of acceptance or paired spellings sharing the same compiler bug.
The bounded executors reject unsupported instructions and missing inputs.
"""
import json
import struct
from pathlib import Path
from rect_matrix_check import execute_fp
from vp_binding_check import evaluate_bindings
from fp_sources import instructions, ucode_words, source, ARITY, INPUT
from vp_words import decode
import re


def check(compile_one):
    rows=json.loads(Path(__file__).with_name('entry_array_value_cases.json').read_text())
    accepts=refusals=0
    for row in rows:
        name='input-elements-'+row['name']
        blob=compile_one(name,row['source'],row['profile'],row.get('refusal'))
        if row.get('refusal'):
            refusals+=1
            continue
        if row['profile']=='sce_fp_rsx':
            actual=execute_fp(blob,[],inputs=row['inputs'])
            used={source(w,j)['name'] for w,c in instructions(ucode_words(blob))
                  for j in range(1,ARITY.get((w[0]>>24)&63,0)+1) if source(w,j)['type']==INPUT}
        else:
            actual,_=evaluate_bindings(blob,{},row['inputs'])
            lines,error=decode(blob)
            assert error is None,error
            used=set()
            for line in lines:
                op=line.split()[1]
                slots={'NOP':[], 'MOV':[0], 'ADD':[0,2], 'MUL':[0,1],
                       'MAD':[0,1,2], 'DP3':[0,1], 'DP4':[0,1]}[op]
                for slot in slots:
                    operand=re.search(r'src'+str(slot)+r'=(\S+)',line).group(1)
                    used.update(re.findall(r'\bIN\d+',operand))
        assert actual==row['expected'],f'{name}: decoded {actual} != {row["expected"]}'
        assert used==set(row['attributes']),f'{name}: attributes {used} != {row["attributes"]}'
        assert actual!=row['wrong'],name+': wrong-value control became equal'
        h=struct.unpack_from('>8I',blob);records={}
        for i in range(h[3]):
            v=struct.unpack_from('>12I',blob,h[4]+48*i)
            def text(o):return blob[o:blob.index(0,o)].decode() if o else ''
            records[text(v[4])]=(v[0],v[1],text(v[7]))
        for key,expected in row['records'].items():
            assert records.get(key)==tuple(expected),f'{name}: record {key}: {records.get(key)} != {expected}'
        accepts+=1
    assert (accepts,refusals)==(58,18),'input binding or boundary row lost'
    return accepts,refusals

#!/usr/bin/env python3
"""Reference-measured member-array/current-selector stores. Out-of-range forwarding
remains an explicitly named reference-accept gap; matrix rows are a later slice.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from rect_matrix_check import execute_vp

def main():
    compiler=str(Path(sys.argv[1]).resolve())
    repo=Path(__file__).resolve().parents[2]
    rows=json.loads(Path(__file__).with_name('member_array_store_cases.json').read_text(encoding='utf-8'))
    root=repo/'.local/tmp';root.mkdir(parents=True,exist_ok=True)
    twins=refusals=0
    with tempfile.TemporaryDirectory(prefix='member-array-',dir=root) as tmp:
        def compile_one(name, source, profile, refusal=None, command=None):
            src=Path(tmp)/(name+'.cg');out=src.with_suffix('.bin')
            src.write_text(source,encoding='utf-8')
            assert not out.exists(), name+': stale output'
            q=subprocess.run((command or [compiler])+['-p',profile,'--emit-container',str(out),str(src)],capture_output=True,text=True,timeout=30)
            log=q.stdout+q.stderr
            if refusal:
                assert q.returncode==1, f'{name}: expected exit 1, got {q.returncode}\n{log}'
                assert not out.exists(), name+': refusal left an output file'
                assert refusal in log, f'{name}: expected {refusal!r}\n{log}'
                return
            assert q.returncode==0, f'{name}: expected acceptance, got {q.returncode}\n{log}'
            assert out.exists(), name+': compiler wrote no container'
            data=out.read_bytes()
            assert len(data)>=32,name+': truncated container'
            h=struct.unpack_from('>8I',data)
            assert h[2]==len(data) and h[6]>0 and h[6]%16==0 and h[7]+h[6]<=len(data),name+': invalid container'
            return data
        for row in rows:
            name=row['name']
            if 'twin' in row:
                a=compile_one(name,row['source'],row['profile'])
                b=compile_one(name+'-explicit',row['twin'],row['profile'])
                assert a==b,name+': strict explicit-expansion twin differs'
                twins+=1
            else:
                compile_one(name,row['source'],row['profile'],row['refusal']);refusals+=1
        outputs=json.loads(Path(__file__).with_name('member_array_output_cases.json').read_text(encoding='utf-8'))
        assert len(outputs)==6, 'output row lost'
        containers={}
        for row in outputs:
            name='output-'+row['name']
            data=compile_one(name,row['source'],'sce_vp_rsx')
            containers[row['name']]=data
            h=struct.unpack_from('>8I',data);records={}
            for i in range(h[3]):
                v=struct.unpack_from('>12I',data,h[4]+48*i)
                def text(off):return data[off:data.index(b'\0',off)].decode() if off else ''
                key=text(v[4])
                assert key not in records,name+': duplicate parameter '+key
                records[key]={'name':key,'semantic':text(v[7]),'type':v[0],'resource':v[1],'index':v[3],'direction':v[8],'referenced':v[10]}
            for key,expected in row['records'].items():
                assert records.get(key)==expected,f'{name}: record {key}: {records.get(key)} != {expected}'
            actual,_=execute_vp(data,{},all_outputs=True)
            assert actual==row['values'],f'{name}: decoded output values/writes {actual} != {row["values"]}'
            assert actual!={'o0':[0]*4},name+': wrong-expectation control failed'
        assert containers['loop']==containers['explicit'],'output loop differs from explicit stores'
        review_rows=json.loads(Path(__file__).with_name('member_array_review_cases.json').read_text(encoding='utf-8'))
        assert len(review_rows)==24,'review row lost'
        for row in review_rows:
            name='review-'+row['name']
            data=compile_one(name,row['source'],row['profile'],row.get('refusal'))
            if 'values' in row:
                actual,_=execute_vp(data,row.get('uniforms',{}),all_outputs=True)
                assert actual==row['values'],f'{name}: decoded {actual} != {row["values"]}'
            if 'records' in row:
                h=struct.unpack_from('>8I',data);records={}
                for i in range(h[3]):
                    v=struct.unpack_from('>12I',data,h[4]+48*i)
                    key=data[v[4]:data.index(b'\0',v[4])].decode()
                    records[key]={'type':v[0],'res':v[1],'referenced':v[10]}
                for key,expected in row['records'].items():
                    assert records.get(key)==expected,f'{name}: record {key}: {records.get(key)} != {expected}'
        # Prove absence, including an empty leaked file, and prove that an
        # exit-zero compiler cannot borrow a prior row's artifact.
        stub=Path(tmp)/'stub.py'
        for name,body,refusal,expected in [
            ('never-writes','raise SystemExit(0)',None,'compiler wrote no container'),
            ('empty-leak',"out.write_bytes(b'');print('NAMED');raise SystemExit(1)",'NAMED','refusal left an output file'),
            ('byte-leak',"out.write_bytes(b'x');print('NAMED');raise SystemExit(1)",'NAMED','refusal left an output file')]:
            stub.write_text("import sys\nfrom pathlib import Path\nout=Path(sys.argv[sys.argv.index('--emit-container')+1])\n"+body+'\n',encoding='utf-8')
            try:
                compile_one(name,'float4 main():COLOR{return 1;}','sce_fp_rsx',refusal,[sys.executable,str(stub)])
            except AssertionError as exc:
                assert expected in str(exc),f'{name}: wrong self-check failure: {exc}'
            else:
                raise AssertionError(name+': broken compiler passed guard')
    assert (twins,refusals)==(42,6),(twins,refusals)
    print(f'PASS ({twins} strict twins, {refusals} named array-store refusals/gaps, 6 output record/value rows, 24 copy/alias/semantic controls)')

if __name__=='__main__':
    try:main()
    except (AssertionError,subprocess.TimeoutExpired) as e:
        print('FAIL: '+str(e),file=sys.stderr);sys.exit(1)

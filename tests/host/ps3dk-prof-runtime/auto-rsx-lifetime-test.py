"""Finite owner/device/sink schedules checked against a separate ownership ledger."""
import argparse
import hashlib
import itertools
import json
import subprocess
from pathlib import Path

def expected(pairs,metadata,actions):
    ledger=[dict(id=0,state=0,complete=0,ack=0) for _ in range(pairs)]
    issued=calls=0;stopped=False
    def submit():
        nonlocal issued,calls
        calls+=1
        if stopped:return 2
        free=[t for t in ledger if t['state'] in (0,5)]
        if not free:return 5
        if sum(t['state'] not in (0,5) for t in ledger)>=metadata:return 6
        issued+=1;free[0].update(id=issued,state=2,complete=0,ack=0)
        return 0
    submit();answer=[]
    for a in actions:
        result=0;reason=99
        t=next((t for t in ledger if t['id']==1),None)
        if a=='P' and not stopped:
            result=1
            for item in ledger:
                if item['state']==2:item['state']=3
        elif a in '01' and t and t['state']==3:
            bit=1<<int(a)
            if not t['complete']&bit:
                result=1;t['complete']|=bit
                if t['complete']==3:t['state']=4
        elif a=='A' and t and t['state']==4:
            result=1;t.update(state=5,ack=1)
        elif a=='N':reason=submit()
        elif a=='T':
            stopped=True
            for item in ledger:
                if item['state'] not in (0,5):item['state']=6
        answer.append([result,reason,issued,calls]+[v for item in ledger for v in (item['id'],item['state'],item['complete'],item['ack'])])
    return answer

def run(binary,out):
    schedules=[''.join(p) for p in itertools.permutations('P01AFNT')]
    schedules+=['P01F'+'N'*32+'A'+'N'*4,'0P1A0AN','P10AN01A','P01ANTN']
    jobs=[(p,m,s) for p in (1,2,3) for m in (0,1,2) for s in schedules]
    data=''.join(f'{p} {m} {s}\n' for p,m,s in jobs).encode()
    r=subprocess.run([str(binary),'schedules'],input=data,capture_output=True)
    (out/'schedules.stderr').write_bytes(r.stderr)
    if r.returncode:raise RuntimeError('schedule executable failed')
    lines=r.stdout.decode().splitlines()
    if len(lines)!=len(jobs):raise AssertionError('incomplete schedule receipt')
    for (p,m,s),line in zip(jobs,lines):
        actual=[[int(x) for x in step.split(',')] for step in line.split(';')]
        want=expected(p,m,s)
        if actual!=want:raise AssertionError(f'schedule {p}/{m}/{s}: {actual} != {want}')
    receipt=dict(schedules=len(jobs),transitions=sum(len(s) for _,_,s in jobs),
        pair_capacities=[1,2,3],metadata_capacities=[0,1,2],permutation_events='P01AFNT',
        input_sha256=hashlib.sha256(data).hexdigest(),output_sha256=hashlib.sha256(r.stdout).hexdigest(),
        scope='Finite sequential state transitions; no C11 ordering or hardware proof')
    (out/'schedules.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('lifetime schedules',receipt['schedules'],'transitions',receipt['transitions'],'PASS')
    return receipt

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--binary',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=False);run(args.binary,args.out)

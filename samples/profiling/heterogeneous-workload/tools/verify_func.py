"""Fixed-tree acceptance; the public decoder must separately accept the full capture.
This checks a known instrumented population, not whole-application coverage.
"""
import argparse
import json
from pathlib import Path

def require(ok,why):
    if not ok:raise ValueError(why)

def unique(items):
    out={}
    for key,value in items:
        require(key not in out,'duplicate JSON key');out[key]=value
    return out

def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'),object_pairs_hook=unique)

def natural(value,name):
    require(type(value) is int and 0<=value<1<<64,name+' must be u64')
    return value

def verify(receipt,report,binding,arm,build,session):
    require(arm in ('ON','OFF'),'diagnostic probe is outside the acceptance gate')
    require(receipt['schema']==1 and receipt['arm']==arm and receipt['build']==build and
            receipt['session']==session,'receipt identity')
    for key,value in dict(frames=64,checked=64,frame_limit=64,iterations_per_frame=64,
                          entries_per_iteration=8,checksum=2144256,rc=0).items():
        require(natural(receipt[key],key)==value,'receipt '+key)
    ticks=natural(receipt['tree_ticks'],'tree ticks');freq=natural(receipt['frequency_hz'],'frequency')
    require(ticks>0 and freq>0,'positive measured interval and frequency')
    require(report['schema']==1 and type(report['capability']) is bool,'report schema')
    if arm=='OFF':
        require(report['capability'] is False,'OFF unexpectedly instrumented')
    else:
        require(report['capability'] is True,'ON missing function capability')
        cap=report['capture']
        require(cap['final_status'] is True and cap['status_saturated'] is False and
                cap['interval_refusal'] is None and natural(cap['no_actor'],'no actor')==0,'capture function coverage')
        require(report['config']['frequency_hz']==freq,'capture/receipt frequency')
        require(report['config']['pointer_bytes']==(4 if binding['abi']=='ilp32' else 8),'ABI')
        actors=[t for t in report['threads'] if t['name']=='main']
        require(len(actors)==1,'one main actor');actor=actors[0]
        require(actor['state']=='DATA' and actor['terminal']=='FINAL','main final data')
        require(not set(actor['flags_seen']) & {'SATURATED','INCOMPLETE_TAIL','STACK_RESET','STACK_UNKNOWN',
                'DEPTH_LIMITED','LOST','CLOCK'},'main coverage flags')
        for key in ('depth_limit','lost','unbalanced','orphan','reentry','clock','abandoned'):
            require(natural(actor['counters'][key],key)==0,'main '+key)
        selection=actor['selection']
        require(selection['kind']=='whole' and selection['state']=='DATA','whole main view')
        view=selection['view'];rows=view['rows']
        require(len(rows)==view['paths_total'],'truncated path list')
        require(view['lost'] is None or natural(view['lost']['calls'],'LOST calls')==0,'LOST calls')
        syms=binding['symbols'];root='sample_func_root';left='sample_func_left';right='sample_func_right';leaf='sample_func_leaf'
        expected={(root,):4096,(root,left):4096,(root,right):4096,
                  (root,left,leaf):8192,(root,right,leaf):12288}
        raw={int(value,16):name for name,value in syms.items()}
        require(len(raw)==4 and set(raw.values())=={root,left,right,leaf},'four distinct ELF descriptors')
        seen={}
        for row in rows:
            path=row['path'];addresses=[int(n['fn'],16) for n in path]
            if not any(a in raw for a in addresses):continue
            require(all(a in raw for a in addresses),'known tree has an unexpected caller')
            names=tuple(raw[a] for a in addresses)
            require(names in expected and names not in seen,'unexpected/duplicate known path')
            require(all(n['symbol']==name for n,name in zip(path,names)),'real ELF symbolization')
            require(row['root']=='root' and row['lost'] is False,'known path attribution')
            require(not set(row['flags']) & {'SATURATED','SELF_UNTIMED','TRUNCATED','LOST'},'known row incomplete')
            calls=natural(row['calls'],'calls');require(calls==expected[names],'known path call count')
            seen[names]=calls
        require(seen==expected,'missing known path')
    return dict(arm=arm,entries=32768,tree_ticks=ticks,frequency_hz=freq,
                mean_us_per_entry=ticks*1000000/freq/32768,
                scope='known-tree elapsed per entry; includes workload and timing; not isolated hook cost')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('receipt');p.add_argument('report');p.add_argument('binding')
    p.add_argument('--arm',choices=['OFF','ON'],required=True);p.add_argument('--build',required=True);p.add_argument('--session',required=True)
    a=p.parse_args()
    try:out=verify(read(a.receipt),read(a.report),read(a.binding),a.arm,a.build,a.session)
    except (ValueError,KeyError,TypeError,OSError) as e:raise SystemExit('REFUSED: '+str(e))
    print(json.dumps(out,sort_keys=True))

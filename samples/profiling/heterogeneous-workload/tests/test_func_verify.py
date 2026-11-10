import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import verify_func as v

def fixture(arm='ON'):
    receipt=dict(schema=1,arm=arm,build='test',session='0000000000000001',frames=64,checked=64,
                 frame_limit=64,iterations_per_frame=64,entries_per_iteration=8,tree_ticks=327680,
                 frequency_hz=1000000,checksum=2144256,rc=0)
    names=['sample_func_root','sample_func_left','sample_func_right','sample_func_leaf']
    symbols={n:hex(4096+8*i) for i,n in enumerate(names)}
    rows=[]
    for path,n in [([0],4096),([0,1],4096),([0,2],4096),([0,1,3],8192),([0,2,3],12288)]:
        rows.append(dict(path=[dict(fn=symbols[names[i]],symbol=names[i]) for i in path],calls=n,
                         root='root',flags=[],lost=False))
    report=dict(schema=1,capability=True,status='RECORDED',capture=dict(final_status=True,
                interval_refusal=None,status_saturated=False,no_actor=0),config=dict(pointer_bytes=4,frequency_hz=1000000),
                threads=[dict(name='main',state='DATA',terminal='FINAL',flags_seen=['DATA','FINAL'],
                    counters={n:0 for n in ['depth_limit','lost','unbalanced','orphan','reentry','clock','abandoned']},
                    selection=dict(kind='whole',state='DATA',view=dict(rows=rows,paths_total=5,lost=None)))])
    if arm=='OFF':report=dict(schema=1,capability=False)
    return receipt,report,dict(abi='ilp32',symbols=symbols)

class Test(unittest.TestCase):
    def test_valid(self):
        for arm in ['OFF','ON']:
            a,b,c=fixture(arm);self.assertEqual(v.verify(a,b,c,arm,'test','0000000000000001')['entries'],32768)
    def test_refusals(self):
        mutations=[lambda a,b,c:a.update(frames=63),lambda a,b,c:a.update(checked=63),
            lambda a,b,c:a.update(checksum=2144255),lambda a,b,c:a.update(tree_ticks=True),
            lambda a,b,c:a.update(rc=1),lambda a,b,c:b['capture'].update(final_status=False),
            lambda a,b,c:b['capture'].update(interval_refusal='rows'),
            lambda a,b,c:b['capture'].update(status_saturated=True),
            lambda a,b,c:b['threads'][0].update(terminal='NOT_RECEIVED'),
            lambda a,b,c:b['threads'][0]['counters'].update(orphan=1),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'][0].update(calls=4095),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'][3]['path'][1].update(fn='0x1010'),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'][0]['path'][0].update(symbol='wrong'),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'].append(copy.deepcopy(b['threads'][0]['selection']['view']['rows'][0])),
            lambda a,b,c:b['threads'][0]['selection']['view'].update(paths_total=6),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'][0].update(flags=['SATURATED']),
            lambda a,b,c:b['threads'][0]['selection']['view']['rows'][0].update(root='unknown_caller')]
        for mutate in mutations:
            a,b,c=fixture();mutate(a,b,c)
            with self.assertRaises(ValueError):v.verify(a,b,c,'ON','test','0000000000000001')

if __name__=='__main__':unittest.main()

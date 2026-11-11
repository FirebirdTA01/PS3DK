import copy
import unittest
from auto_rsx_read import coverage, projection

def fixture():
    return dict(coverage_lost=0, frames=[dict(owner=1, frame=0, closed=1,
        unsupported=2, kinds=[dict(visited=10, observed=9, excluded=[0,0,0,0,0,0,0,1,0]) for _ in range(3)])])

class Reader(unittest.TestCase):
    def test_frame_coverage(self):
        row=coverage(fixture())[0]
        self.assertEqual((row['owner'],row['frame'],row['visited'],row['observed'],row['excluded']), (1,0,30,27,3))
        self.assertEqual(row['status'],'COVERED_POPULATION_ONLY')
        self.assertTrue(row['needs_room_plan'])
    def test_per_kind_coverage(self):
        x=fixture();a=coverage(x)[0]
        x['frames'][0]['kinds'][0]['excluded'][7]=2;x['frames'][0]['kinds'][0]['observed']=8
        x['frames'][0]['kinds'][1]['excluded'][7]=0;x['frames'][0]['kinds'][1]['observed']=10
        b=coverage(x)[0]
        self.assertNotEqual(a,b)
        self.assertEqual(b['kinds']['ARRAY']['excluded'],2)
    def test_no_silent_empty(self):
        x=fixture()
        for k in x['frames'][0]['kinds']:k['observed']=0;k['excluded'][7]=10
        self.assertEqual(coverage(x)[0]['excluded'],30)
        x['coverage_lost']=1
        self.assertEqual(coverage(x)[0]['status'],'INCOMPLETE')
    def test_saturated_counts_never_exact(self):
        x=fixture();x['coverage_lost']=1
        x['frames'][0]['kinds'][0].update(visited=(1<<64)-1,observed=(1<<64)-1)
        row=coverage(x)[0]
        self.assertEqual(row['status'],'INCOMPLETE');self.assertIsNone(row['needs_room_plan'])
    def test_bad_accounting(self):
        mutations=[lambda x:x['frames'].append(copy.deepcopy(x['frames'][0])),
            lambda x:x['frames'][0]['kinds'][0].update(visited=11),
            lambda x:x['frames'][0]['kinds'][0].update(observed=True),
            lambda x:x['frames'][0]['kinds'][0]['excluded'].__setitem__(0,1),
            lambda x:x['frames'][0].update(owner=-1),
            lambda x:x['frames'][0]['kinds'][0].update(excluded=[])]
        for mutate in mutations:
            x=fixture();mutate(x)
            with self.assertRaises(ValueError):coverage(x)
    def test_projection(self):
        app=[0x00040100,0]
        start=[0x00041800,0x01000000,0x00041d6c,3840,0x00041d70,0x10000]
        end=[0x00041800,0x01000010,0x00041d6c,3840,0x00041d70,0x20000]
        self.assertEqual(projection(app,start+app+end),app)
        for x in (start+app+app+end,start+end,start+app+end[:-1]):
            with self.assertRaises(ValueError):projection(app,x)

if __name__=='__main__':unittest.main()

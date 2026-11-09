import copy
import errno
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('verify', Path(__file__).parents[1]/'tools/verify.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
BUILD = 'a' * 64
SESSION = '0000000000001234'
TARGET_TIMEOUT = -2147418101

def receipt(arm='ON'):
    lines = [f'WAIT_SAMPLE schema=1 arm={arm} build={BUILD} session={SESSION}',
             'COUNTS mutex=2050 timed_mutex=64 condition_timeout=16 worker_mutex=32 rows=82', 'SHUTDOWN rc=0']
    for i in range(82):
        kind = 'mutex' if i < 64 else 'timed_mutex' if i < 66 else 'condition_timeout'
        calls = 32 if i < 66 else 1
        lines.append(f'ROW index={i} kind={kind} calls={calls} begin={i*100} end={i*100+50}')
    return '\n'.join(lines + ['WAIT_SAMPLE_END complete=1']) + '\n'

def report(timeout=TARGET_TIMEOUT):
    rows=[]
    summary=[]
    for actor, name, counts in [(1,'init',[(1,2050),(2,64),(3,16)]),(2,'preexisting-worker',[(1,32)])]:
        seq=1
        for op, n in counts:
            for _ in range(n):
                rows.append(dict(actor=actor,thread=name,operation={1:'pthread_mutex_lock',2:'pthread_mutex_timedlock',3:'pthread_cond_wait/timedwait'}[op],
                    call=seq,target_raw=19,secondary_raw=20 if op==3 else 0,timeout_raw=20 if op!=1 else 0,
                    site_raw=1024,elapsed_ticks=50,raw_result=timeout if op in (2,3) else 0,api_result=timeout if op in (2,3) else 0,state='RETURNED',dependency='UNKNOWN: raw handle, no lifecycle proof'))
                seq+=2
        summary.append(dict(actor=actor,thread=name,state='ELIGIBLE; counts require recorded status',summary=dict(flags=0,published=seq-1,last_seq=seq-1,dropped=0,refused=0,nested=0,unregistered=0,attempted_seq=seq-1)))
    return dict(schema=1,capability=True,coverage_status='RECORDED',refusal=None,frequency_hz=79800000,max_actors=64,
        rows=rows,coverage=summary,unregistered_summary=dict(flags=1,published=0,last_seq=0,dropped=0,refused=3,nested=3,internal_excluded=3,unregistered=0,attempted_seq=0))

class Verify(unittest.TestCase):
    def test_timeout_is_explicit_and_target_specific(self):
        for timeout in [TARGET_TIMEOUT, errno.ETIMEDOUT]:
            self.assertEqual(verify.check(receipt(),report(timeout),'ON',BUILD,SESSION,timeout)['calls'],2162)
        for wrong in [116, errno.ETIMEDOUT]:
            with self.subTest(wrong=wrong), self.assertRaisesRegex(ValueError,'timed mutex timeout'):
                verify.check(receipt(),report(wrong),'ON',BUILD,SESSION,TARGET_TIMEOUT)
        with self.assertRaises(TypeError):
            verify.check(receipt(),report(),'ON',BUILD,SESSION)
        for invalid in [0,True,None,'-2147418101',-(1<<31)-1,1<<31]:
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError,'expected timeout'):
                verify.check(receipt(),report(),'ON',BUILD,SESSION,invalid)
    def test_condition_rejects_non_target_timeout(self):
        r=report()
        next(x for x in r['rows'] if x['operation']=='pthread_cond_wait/timedwait')['api_result']=116
        with self.assertRaisesRegex(ValueError,'condition timeout'):
            verify.check(receipt(),r,'ON',BUILD,SESSION,TARGET_TIMEOUT)
    def test_timed_mutex_must_timeout(self):
        for raw,api in [(0,0),(-1,45),(0,TARGET_TIMEOUT)]:
            r=report()
            row=next(x for x in r['rows'] if x['operation']=='pthread_mutex_timedlock')
            row.update(raw_result=raw,api_result=api)
            with self.assertRaises(ValueError): verify.check(receipt(),r,'ON',BUILD,SESSION,TARGET_TIMEOUT)
    def test_observed_init_name_is_required(self):
        r=report()
        for row in r['rows']:
            if row['thread']=='init': row['thread']='main'
        r['coverage'][0]['thread']='main'
        with self.assertRaises(ValueError): verify.check(receipt(),r,'ON',BUILD,SESSION,TARGET_TIMEOUT)
    def test_good(self):
        self.assertEqual(verify.check(receipt(),report(),'ON',BUILD,SESSION,TARGET_TIMEOUT)['calls'],2162)
    def test_off(self):
        r=dict(schema=1,capability=False,coverage_status='UNSUPPORTED',refusal=None,rows=[],coverage=[],unregistered_summary=None)
        self.assertEqual(verify.check(receipt('OFF'),r,'OFF',BUILD,SESSION,TARGET_TIMEOUT)['calls'],0)
    def test_receipt_refusals(self):
        for bad in [receipt().replace('mutex=2050','mutex=2049'),receipt().replace('rc=0','rc=1'),receipt().replace('complete=1','complete=0'),receipt()+receipt(),receipt().replace('index=1 ','index=0 '),receipt().replace('begin=0 end=50','begin=51 end=50'),receipt().replace(BUILD,'b'*64),receipt().replace(SESSION,'0000000000004321'),receipt().replace('calls=32','calls=31',1)]:
            with self.subTest(bad=bad[:100]), self.assertRaises(ValueError): verify.check(bad,report(),'ON',BUILD,SESSION,TARGET_TIMEOUT)
    def test_report_refusals(self):
        mutations=[lambda r:r['rows'].pop(),lambda r:r['rows'].append(copy.deepcopy(r['rows'][0])),lambda r:r['rows'][0].update(state='OPEN',elapsed_ticks=None),
            lambda r:r['rows'][0].update(site_raw=0),lambda r:r['rows'][0].update(elapsed_ticks=True),lambda r:r['rows'][0].update(api_result=1),
            lambda r:r['coverage'][0]['summary'].update(dropped=1),lambda r:r['coverage'][0]['summary'].update(flags=5),
            lambda r:r.update(coverage_status='INCOMPLETE'),lambda r:r.update(frequency_hz=0),lambda r:r.update(refusal='budget'),
            lambda r:r['rows'][0].update(thread='other'),lambda r:r['rows'][0].update(dependency='MATCHED'),lambda r:r['unregistered_summary'].update(refused=1),
            lambda r:r.update(schema=True),lambda r:r['coverage'][0]['summary'].update(flags=False),lambda r:r['rows'][0].update(operation={})]
        for mutate in mutations:
            r=report();mutate(r)
            with self.subTest(mutation=mutations.index(mutate)), self.assertRaises(ValueError): verify.check(receipt(),r,'ON',BUILD,SESSION,TARGET_TIMEOUT)

if __name__=='__main__': unittest.main()

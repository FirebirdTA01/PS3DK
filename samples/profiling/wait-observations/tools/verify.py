"""Count/identity gate for the fixed no-zone sample. Timing is descriptive only.

Input JSON must be the pinned reader's `waits --json` output from the paired
capture. The external run manifest binds capture hash, session, reader and SELF.
This verifier does not establish that binding by looking at a filename.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

def need(ok, reason):
    if not ok:
        raise ValueError(reason)

def integer(x, minimum=0, maximum=(1 << 64)-1):
    return type(x) is int and minimum <= x <= maximum

def check(text, report, arm, build, session, expected_timeout):
    need(integer(expected_timeout,-(1<<31),(1<<31)-1) and expected_timeout != 0, 'expected timeout')
    need(arm in ('OFF','ON'), 'arm')
    need(re.fullmatch('[0-9a-f]{64}', build) is not None, 'build pin')
    need(re.fullmatch('[0-9a-f]{16}', session) is not None, 'session pin')
    lines = text.splitlines()
    need(len(lines) == 86, 'receipt line count')
    need(lines[0] == f'WAIT_SAMPLE schema=1 arm={arm} build={build} session={session}', 'receipt identity')
    need(lines[1] == 'COUNTS mutex=2050 timed_mutex=64 condition_timeout=16 worker_mutex=32 rows=82', 'receipt counts')
    need(lines[2] == 'SHUTDOWN rc=0' and lines[-1] == 'WAIT_SAMPLE_END complete=1', 'receipt completion')
    ticks = {'mutex': [], 'timed_mutex': [], 'condition_timeout': []}
    previous = 0
    for index, line in enumerate(lines[3:-1]):
        m = re.fullmatch(r'ROW index=(\d+) kind=(mutex|timed_mutex|condition_timeout) calls=(\d+) begin=(\d+) end=(\d+)', line)
        need(m is not None, 'row grammar')
        i, kind, calls, begin, end = m.groups()
        i, calls, begin, end = map(int, (i,calls,begin,end))
        expected = 'mutex' if index < 64 else 'timed_mutex' if index < 66 else 'condition_timeout'
        need(i == index and kind == expected and calls == (32 if index < 66 else 1), 'row ledger')
        need(integer(begin) and integer(end) and previous <= begin <= end, 'clock regression/range')
        previous = end
        ticks[kind].append(dict(calls=calls, ticks=end-begin))
    need(type(report) is dict and type(report.get('schema')) is int and report.get('schema') == 1 and report.get('refusal') is None, 'reader refused/schema')
    if arm == 'OFF':
        need(report.get('capability') is False and report.get('coverage_status') == 'UNSUPPORTED', 'OFF capability')
        need(report.get('rows') == [] and report.get('coverage') == [] and report.get('unregistered_summary') is None, 'OFF observations')
        return dict(calls=0, outer_elapsed_ticks=ticks)
    need(report.get('capability') is True and report.get('coverage_status') == 'RECORDED', 'ON status')
    need(integer(report.get('frequency_hz'), 1) and report.get('max_actors') == 64, 'clock/capacity')
    expected = {('init','pthread_mutex_lock'):2050, ('init','pthread_mutex_timedlock'):64,
                ('init','pthread_cond_wait/timedwait'):16, ('preexisting-worker','pthread_mutex_lock'):32}
    counts=Counter(); actors={}; seen=set()
    rows=report.get('rows'); coverage=report.get('coverage')
    need(type(rows) is list and len(rows) == 2162 and type(coverage) is list and len(coverage) == 2, 'population')
    for row in rows:
        need(type(row) is dict, 'row type')
        actor=row.get('actor'); name=row.get('thread'); call=row.get('call'); op=row.get('operation')
        need(op in ('pthread_mutex_lock','pthread_mutex_timedlock','pthread_cond_wait/timedwait'), 'operation')
        need(integer(actor,1,64) and integer(call,1) and call % 2 == 1, 'call identity')
        need(name in ('init','preexisting-worker') and actors.setdefault(actor,name) == name, 'actor identity')
        need((actor,call) not in seen, 'duplicate call');seen.add((actor,call))
        need(row.get('state') == 'RETURNED' and integer(row.get('elapsed_ticks')), 'unavailable elapsed')
        need(row.get('dependency') == 'UNKNOWN: raw handle, no lifecycle proof', 'dependency overclaim')
        need(integer(row.get('site_raw'),1) and integer(row.get('target_raw'),1), 'missing site/target')
        need(integer(row.get('secondary_raw')) and integer(row.get('timeout_raw')), 'raw identity/timeout')
        raw=row.get('raw_result'); api=row.get('api_result')
        need(integer(raw,-(1<<31),(1<<31)-1) and integer(api,-(1<<31),(1<<31)-1), 'result type')
        if op == 'pthread_cond_wait/timedwait':
            need(api == expected_timeout and raw != 0 and row['secondary_raw'] != 0 and row['timeout_raw'] != 0, 'condition timeout')
        elif op == 'pthread_mutex_timedlock':
            need(api == expected_timeout and raw != 0 and row['secondary_raw'] == 0 and row['timeout_raw'] != 0, 'timed mutex timeout')
        else:
            need(api == 0 and raw == 0 and row['secondary_raw'] == 0, 'lock result')
        counts[name,op]+=1
    need(dict(counts) == expected and len(actors) == 2 and len(set(actors.values())) == 2, 'operation ledger')
    actor_counts=Counter(row['actor'] for row in rows)
    covered=set()
    for c in coverage:
        need(type(c) is dict and integer(c.get('actor'),1,64) and c.get('actor') in actors and c.get('actor') not in covered, 'coverage identity')
        actor=c['actor'];covered.add(actor)
        need(c.get('thread') == actors[actor] and c.get('state') == 'ELIGIBLE; counts require recorded status', 'coverage state')
        s=c.get('summary'); n=actor_counts[actor]*2
        need(type(s) is dict and all(integer(x) for x in s.values()) and s == dict(flags=0,published=n,last_seq=n,dropped=0,refused=0,nested=0,unregistered=0,attempted_seq=n), 'actor counters')
        need({call for a,call in seen if a==actor} == set(range(1,n,2)), 'sequence coverage')
    aggregate=report.get('unregistered_summary')
    need(type(aggregate) is dict and all(integer(x) for x in aggregate.values()), 'aggregate types')
    internal=aggregate.get('internal_excluded')
    need(integer(internal,1) and aggregate == dict(flags=1,published=0,last_seq=0,dropped=0,refused=internal,nested=internal,internal_excluded=internal,unregistered=0,attempted_seq=0), 'aggregate counters')
    return dict(calls=len(rows), internal_excluded=internal, frequency_hz=report['frequency_hz'], outer_elapsed_ticks=ticks)

def unique(pairs):
    result={}
    for key,value in pairs:
        need(key not in result, 'duplicate JSON key')
        result[key]=value
    return result

if __name__ == '__main__':
    p=argparse.ArgumentParser()
    p.add_argument('receipt',type=Path);p.add_argument('waits_json',type=Path)
    p.add_argument('--arm',choices=['OFF','ON'],required=True)
    p.add_argument('--build',required=True);p.add_argument('--session',required=True)
    p.add_argument('--expected-timeout',type=int,required=True,help='ETIMEDOUT derived from the paired target build; pin it in the run manifest')
    a=p.parse_args()
    result=check(a.receipt.read_text(),json.loads(a.waits_json.read_text(),object_pairs_hook=unique),a.arm,a.build,a.session,a.expected_timeout)
    print(json.dumps(result,indent=2))

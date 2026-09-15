#!/usr/bin/env python3
"""VP literal folds, measured on sce-cgc 475; controls contain decoded values.

sin/cos round a double result once, tan divides separately rounded sin/cos,
sqrt rounds reciprocal-sqrt before taking its reciprocal. These are distinct
from sinf/cosf/tanf/sqrtf at the named separating inputs below. Whole reference
containers equal each explicit-value twin; candidate twins must also be exact.
Large trig arguments are our named reduction gap, not a reference refusal.
"""
import argparse
import pathlib
import struct
import subprocess
import tempfile

TWINS = [
    ('sin-double-negative', 'float4(sin(-59014.0625f),0,0,1)', 'float4(-0.685651004f,0,0,1)'),
    ('sin-double-positive', 'float4(sin(31215.625f),0,0,1)', 'float4(0.689206898f,0,0,1)'),
    ('cos-double-negative', 'float4(cos(-6645.2587890625f),0,0,1)', 'float4(-0.703613162f,0,0,1)'),
    ('cos-double-positive', 'float4(cos(7945.87939453125f),0,0,1)', 'float4(-0.702726603f,0,0,1)'),
    ('tan-rounded-division', 'float4(tan(0.3f),0,0,1)', 'float4(0.309336245f,0,0,1)'),
    ('tan-rounded-division-negative', 'float4(tan(3.0f),0,0,1)', 'float4(-0.142546535f,0,0,1)'),
    ('sqrt-rounded-reciprocal', 'float4(sqrt(3.0f),0,0,1)', 'float4(1.7320509f,0,0,1)'),
    ('sqrt-rounded-reciprocal-six', 'float4(sqrt(6.0f),0,0,1)', 'float4(2.44948959f,0,0,1)'),
    ('sqrt-integer-argument', 'float4(sqrt(3),0,0,1)', 'float4(1.7320509f,0,0,1)'),
    ('tan-integer-argument', 'float4(tan(3),0,0,1)', 'float4(-0.142546535f,0,0,1)'),
    ('sqrt-half-vector', 'float4(sqrt(half2(2,3)),0,1)', 'float4(1.41421356f,1.7320509f,0,1)'),
    ('tan-vector', 'float4(tan(float2(.3,3)),0,1)', 'float4(.309336245f,-.142546535f,0,1)'),
    ('sqrt-mixed-lanes', 'sqrt(float4(0.0,-0.0,-1.0,4.0))', 'float4(sqrt(0.0),sqrt(-0.0),sqrt(-1.0),2.0)'),
    ('sin-boundary', 'float4(sin(65536.0f),0,0,1)', 'float4(0.692065477f,0,0,1)'),
    ('cos-boundary', 'float4(cos(65536.0f),0,0,1)', 'float4(-0.721834779f,0,0,1)'),
    ('tan-boundary', 'float4(tan(65536.0f),0,0,1)', 'float4(-0.958758831f,0,0,1)'),
]
GAPS = [(op+'-'+tag, 'float4('+op+'('+x+'),0,0,1)')
        for op in ('sin','cos','tan')
        for tag,x in [('next','65536.0078125f'),('negative-next','-65536.0078125f'),
                      ('integer-next','65537'),('negative-integer-next','-65537'),
                      ('sampled-large','1e12f'),('large','1e30f'),('nonfinite','1e39f')]]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    parser.add_argument('--reference', action='store_true')
    args = parser.parse_args()
    compiler = str(pathlib.Path(args.compiler).resolve())
    scratch = pathlib.Path(__file__).resolve().parents[2]/'.local'/'tmp'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vp-literals-', dir=scratch) as tmp:
        work = pathlib.Path(tmp)
        def compile_one(name, expression, refusal=False):
            src = work/(name+'.cg'); dst = work/(name+'.vpo')
            src.write_text('float4 main():POSITION{return '+expression+';}')
            assert not dst.exists(), name+': stale output'
            command = [compiler,'-p','sce_vp_rsx', '-o' if args.reference else '--emit-container',str(dst),str(src)]
            run = subprocess.run(command, capture_output=True, text=True, timeout=60)
            diagnostic = run.stdout+run.stderr
            if refusal and args.reference and name.endswith('-nonfinite'):
                assert run.returncode != 0 and not dst.exists() and 'C0123' in diagnostic, name+': expected reference literal-overflow refusal'
                return None
            if refusal and not args.reference:
                assert run.returncode == 1 and not dst.exists(), name+': expected exit 1 and no output file: '+diagnostic
                assert 'our VP constant trig reduction bound' in diagnostic and 't_b939dc41' in diagnostic, name+': wrong refusal: '+diagnostic
                return None
            assert run.returncode == 0 and dst.is_file(), name+': expected acceptance: '+diagnostic
            data = dst.read_bytes()
            assert len(data) >= 32, name+': short container'
            header = struct.unpack_from('>8I', data)
            assert header[2] == len(data) and header[6] > 0 and header[6] % 16 == 0 and header[7]+header[6] <= len(data), name+': invalid container'
            return data
        for name,expr,twin in TWINS:
            assert compile_one(name,expr) == compile_one(name+'-twin',twin), name+': decoded reference-value twin differs'
        for name,expr in GAPS:
            compile_one(name,expr,refusal=True)
    assert len(TWINS) == 16 and len(GAPS) == 21
    print('PASS: VP literal folds (16 strict twins, '+('18 reference-accept gap controls, 3 reference overflow refusals' if args.reference else '21 named reduction-bound refusals')+')')

if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as error:
        raise SystemExit('FAIL: '+str(error))

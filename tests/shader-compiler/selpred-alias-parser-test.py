#!/usr/bin/env python3
"""t_3603033d: test record boundaries in both directions before trusting a trace."""
import pathlib
import subprocess
import sys
import tempfile

checker = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name('selpred_alias_check.py')

def trace(dst=8, src0=5, src1=1, following=8, half=False):
    return f'''alloc[98] op=29 opName=SelPred dstOut=0 dstIdx=79 dstPhys={dst} dstFp16=0 preferredPhys=-1
  src0 kind=1 idx=11 phys={src0} fp16={int(half)} swz=0000 neg=0 abs=0
  src1 kind=1 idx=25 phys={src1} fp16=0 swz=0000 neg=0 abs=0
  src2 kind=4 idx=0 phys=-1 fp16=0 swz=0000 neg=0 abs=0
alloc[99] op=0 opName=Mov dstOut=1 dstIdx=1 dstPhys=-1 dstFp16=0 preferredPhys=-1
  src0 kind=1 idx=79 phys={following} fp16=0 swz=0000 neg=0 abs=0
  src1 kind=0 idx=0 phys=-1 fp16=0 swz=0123 neg=0 abs=0
  src2 kind=0 idx=0 phys=-1 fp16=0 swz=0123 neg=0 abs=0
'''

# Later MOV sources must neither invent nor erase a SelPred collision.
cases = [
    ('safe-consumer', trace(), True, '2 early temp sources'),
    ('collision-before-safe-mov', trace(src0=8, following=5), False, 'early-read src0 v11'),
    ('then-source-collision', trace(src1=8, following=5), False, 'early-read src1 v25'),
    ('half-source-collision', trace(src0=17, following=5, half=True), False, 'early-read src0 v11'),
    ('unresolved-source', trace(src0=-1, following=5), False, 'unresolved'),
    ('unresolved-destination', trace(dst=-1, following=5), False, 'unresolved destination'),
    ('incomplete-record', trace().replace('  src1 kind=1 idx=25 phys=1 fp16=0 swz=0000 neg=0 abs=0\n', ''), False, 'incomplete'),
]
failures = []
with tempfile.TemporaryDirectory() as tmp:
    for name, text, want_ok, evidence in cases:
        path = pathlib.Path(tmp) / (name + '.log')
        path.write_text(text, encoding='utf-8')
        result = subprocess.run([sys.executable, str(checker), str(path)], capture_output=True, text=True)
        message = result.stdout + result.stderr
        if (result.returncode == 0) != want_ok or evidence not in message:
            failures.append(f'{name}: expected {"PASS" if want_ok else "FAIL"} containing {evidence!r}; exit={result.returncode}: {message.strip()}')
if failures:
    raise SystemExit('\n'.join(failures))
print(f'selpred-alias-parser-test: PASS ({len(cases)} cases)')

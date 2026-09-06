#!/usr/bin/env bash
# Left-associated arithmetic and step sums must use bounded registers as N
# grows. Reference default O1 uses two registers for both independent shapes
# at N=2..40; the old schedule materializes every term before adding it.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 - "$compiler" "$repo_root" <<'PY'
import pathlib, struct, subprocess, sys, tempfile
compiler, root = sys.argv[1:]
shaders = pathlib.Path(root) / 'tools/rsx-cg-compiler/tests/shaders/accumulation'
errors = []
with tempfile.TemporaryDirectory(prefix='ps3dk-accumulation-') as work:
    for shape in ('mad', 'step'):
        counts = {}
        for n in (2, 8, 24, 40):
            source = shaders / ('%s_n%02d_f.cg' % (shape, n))
            output = pathlib.Path(work) / ('%s_%d.fpo' % (shape, n))
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container',
                                  str(output), str(source)], capture_output=True,
                                 text=True, timeout=30)
            if run.returncode:
                errors.append('%s N=%d refused: %s' %
                              (shape, n, (run.stdout + run.stderr).strip()))
                continue
            blob = output.read_bytes()
            program = struct.unpack_from('>I', blob, 20)[0]
            counts[n] = blob[program + 18]
        print('%s register counts: %s' % (shape, counts))
        # Fixed scratch allowance independent of N; do not encode the
        # reference's exact count or accept a merely slower linear growth.
        if counts and max(counts.values()) > 8:
            errors.append('%s exceeds the fixed eight-register bound: %s' % (shape, counts))
        if 8 in counts and any(counts[n] != counts[8] for n in (24, 40) if n in counts):
            errors.append('%s register count grows after N=8: %s' % (shape, counts))
if errors:
    raise SystemExit('\n'.join('FAIL: ' + error for error in errors))
print('accumulation order: bounded registers and N=40 step compiles')
PY

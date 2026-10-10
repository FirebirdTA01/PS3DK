#!/usr/bin/env bash
# A long predicated fragment program must not be refused for register
# pressure the instruction scheduler created.
#
# The fragment scheduler is latency-driven and pulls independent work early.
# On the Mossbrook skyBox shape (five unrolled layers of atan2 UVs,
# derivatives and two gradient texture fetches each, all under ifs) its
# order kept 114 temps live at once, past the 48 the encoding can address,
# and the program was refused; the order the program was lowered in needs
# 17.  The scheduler now keeps the lowered order when its own order would
# not fit and the lowered one would.
#
# Checks: the reproducer compiles (exit 0, container written) and its
# declared FP register count is under 48.  The v0.22.0 compiler, which
# refuses it, is the red control.
#
# Usage: tests/shader-compiler/fp-schedule-pressure-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-schedule-pressure: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/mossbrook-repros/d_skybox_register_pressure.fcg"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

"$compiler" -p sce_fp_rsx -e main --emit-container "$work/sky.fpo" "$src" > "$work/log.txt" 2>&1
rc=$?
if [ $rc -ne 0 ] || [ ! -s "$work/sky.fpo" ]; then
    echo "fp-schedule-pressure: FAIL: skyBox reproducer refused (rc $rc)"
    grep -iE "error|refus" "$work/log.txt" | head -3 | sed 's/^/    /'
    exit 1
fi
python3 - "$work/sky.fpo" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
prog = struct.unpack_from('>I', b, 20)[0]
regs = b[prog + 18]
ok = 0 < regs < 48
print('fp-schedule-pressure: %s skyBox reproducer compiles with %d FP registers (limit 47)'
      % ('ok  ' if ok else 'FAIL', regs))
sys.exit(0 if ok else 1)
EOF
st=$?
[ $st -eq 0 ] && echo "fp-schedule-pressure: PASS" || echo "fp-schedule-pressure: FAIL"
exit $st

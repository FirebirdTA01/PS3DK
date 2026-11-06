#!/usr/bin/env python3
"""Live link end to end on loopback: the real C agent (host build) serving a
growing capture to the viewer's real live client. Host evidence only."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path, help='a new directory for the build and logs')
p.add_argument('--runs', type=int, default=3)
a = p.parse_args()
if a.runs < 1:
    p.error('--runs must be at least 1')
root = Path(__file__).resolve().parents[3]
src = root / 'sdk/libps3dkprof/src'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
serve = out / 'live-agent-serve'
cc = [os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
      '-DPS3DK_PROF_PAL_HOST', '-I', inc.parent, '-I', src / 'core', '-I', src / 'pal', '-I', src / 'live',
      src / 'live/live_agent.c', src / 'live/live_os_host.c', src / 'pal/pal_host.c',
      Path(__file__).parent / 'live_agent_serve.c', '-o', serve]
r = subprocess.run([str(x) for x in cc], capture_output=True, text=True)
if r.returncode != 0:
    raise SystemExit('serve compile failed:\n' + r.stderr)
# The public-API serve: the real runtime (FILE sink) and ps3dk_prof_live_start/stop.
api_serve = out / 'live-api-serve'
cc = [os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
      '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-I', inc.parent, '-I', src / 'core',
      '-I', src / 'pal', '-I', src / 'live', src / 'core/prof_core.c', src / 'core/prof_drain.c',
      src / 'pal/pal_host.c', src / 'live/live_agent.c', src / 'live/live_os_host.c', src / 'live/live_api.c',
      Path(__file__).parent / 'live_api_serve.c', '-o', api_serve]
r = subprocess.run([str(x) for x in cc], capture_output=True, text=True)
if r.returncode != 0:
    raise SystemExit('api serve compile failed:\n' + r.stderr)
failed = 0
for i in range(1, a.runs + 1):
    r = subprocess.run(['cargo', 'test', '-p', 'ps3dk-prof-viewer', '--test', 'live_e2e', '--', '--ignored',
                        '--nocapture'], cwd=root / 'tools', capture_output=True, text=True, timeout=600,
                       env=dict(os.environ, LIVE_AGENT_SERVE=str(serve), LIVE_API_SERVE=str(api_serve)))
    (out / f'run-{i}.log').write_text(r.stdout + r.stderr)
    ok = r.returncode == 0 and 'test result: ok. 2 passed' in r.stdout
    failed += not ok
    for line in (r.stdout + r.stderr).splitlines():
        if line.startswith('E2E'):  # E2E (agent) and E2E-API (the public API over the runtime)
            print(f'run {i}: {line}')
    print(f'run {i}: {"PASS" if ok else "FAIL"}')
print(f'live-e2e: {a.runs - failed}/{a.runs} passed')
raise SystemExit(1 if failed else 0)

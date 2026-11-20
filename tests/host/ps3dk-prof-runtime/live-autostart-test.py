#!/usr/bin/env python3
"""The opt-in live viewer auto-start (live/live_autostart.c) on the host: the runtime cases of live_autostart_test.c
(plain and ASan+UBSan, one process per case, each case's one-line diagnostics checked), then the opt-in itself:
the header's request and options macros (C and C++, on and off builds) and an archive link that extracts the
auto-start member only when requested.  usage: live-autostart-test.py <new output dir>"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path, help='a new directory for the builds')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
src = root / 'sdk/libps3dkprof/src'
net = root / 'sdk/libnet/src'
here = Path(__file__).parent
runtime = [src / 'core/prof_core.c', src / 'core/prof_drain.c', src / 'core/prof_version.c', src / 'pal/pal_host.c',
           src / 'rolling/roll_file.c', src / 'rolling/roll_retention_inspect.c', src / 'rolling/roll_store.c',
           src / 'live/live_agent.c', src / 'live/live_os_host.c', src / 'live/live_api.c']
autostart = src / 'live/live_autostart.c'
cc = os.environ.get('CC', 'cc')
cxx = os.environ.get('CXX', 'c++')
# -no-pie: libnet's holder layer refuses a pool above 4 GiB, as the console's address space never has one.
base = ['-std=c11', '-O2', '-g', '-no-pie', '-Wall', '-Wextra', '-Werror', '-Wno-prio-ctor-dtor', '-pthread',
        '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-I', str(inc.parent),
        '-I', str(src / 'core'), '-I', str(src / 'pal'), '-I', str(src / 'live'), '-I', str(net)]
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
       '-ftls-model=local-exec']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', ASAN_OPTIONS='detect_leaks=0')
REPORTS = ['runtime error:', 'ERROR: AddressSanitizer', 'SUMMARY: UndefinedBehaviorSanitizer',
           'SUMMARY: AddressSanitizer']
LISTEN = 'ps3prof: live viewer: connect to this console on port '
# per case: (stderr must contain, stdout after PASS must contain)
EXPECT = {
    'stream_app': (LISTEN, None),
    'stream_thread': (LISTEN, None),
    'sole_holder': (LISTEN, None),
    'evict_legacy_owner': ('ps3prof: live auto-start stopped: the app took the network back', None),
    'evict_cell_owner': ('ps3prof: live auto-start stopped: the app took the network back', None),
    'evict_prof_then_cell': ('libnet: the app initialized the network with its own pool', None),
    'net_unavailable': ('ps3prof: live auto-start off: network bring-up failed (0x80010224)', None),
    'listen_fails': ('ps3prof: live auto-start off: cannot listen on port 31750', None),
    'not_servable': ('ps3prof: live auto-start off: the capture cannot be served', None),
    'thread_fail': ('ps3prof: live auto-start off: start-up thread not created', None),
    'explicit_compat': (LISTEN, None),
    'app_first': ('ps3prof: live auto-start: the app started the live agent itself', None),
    'start_during_bringup': ('ps3prof: live auto-start: the app started the live agent itself', None),
    'shutdown_during_bringup': ('ps3prof: live auto-start: start-up finished after teardown; network hold given back',
                                None),
    'start_stuck_bounded': ('ps3prof: live auto-start: start-up thread still in network bring-up; left to finish', None),
    'stop_timeout_keeps_hold': ('ps3prof: live auto-start: the agent did not stop (19); the network stays up', None),
    'exit_without_shutdown': (LISTEN, 'NET_RELEASE rc=0 net_up=1'),
}

failed = 0
for label, extra in [('plain', []), ('sanitize', san)]:
    exe = out / f'live-autostart-{label}'
    r = subprocess.run([cc, *base, *extra, *map(str, runtime), str(autostart), str(net / 'net_hold.c'),
                        str(net / 'net_pool.c'), str(here / 'live_autostart_test.c'), '-o', str(exe)],
                       capture_output=True, text=True)
    (out / f'{label}.compile.log').write_text(r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'{label} compile failed:\n{r.stderr}')
    cases = subprocess.run([str(exe), '--list'], capture_output=True, text=True, check=True).stdout.split()
    assert sorted(cases) == sorted(EXPECT), f'cases {cases} vs expectations {sorted(EXPECT)}'
    passed = 0
    for name in cases:
        d = out / f'{label}-{name}'
        d.mkdir()
        r = subprocess.run([str(exe), name, str(d)], capture_output=True, text=True, timeout=120, env=env)
        (d / 'stdout').write_text(r.stdout)
        (d / 'stderr').write_text(r.stderr)
        want_err, want_after = EXPECT[name]
        reports = [x for x in REPORTS if x in r.stderr]
        after = r.stdout.split(f'PASS {name}', 1)[1] if f'PASS {name}' in r.stdout else ''
        ok = (r.returncode == 0 and f'PASS {name}' in r.stdout and not reports and 'FAIL' not in r.stderr
              and want_err in r.stderr and (want_after is None or want_after in after))
        passed += ok
        for line in r.stdout.splitlines():
            if line.startswith('MEASURE'):
                print(f'{label} {name}: {line}')
        if not ok:
            print(f'FAIL {label} {name} rc={r.returncode} reports={reports} stderr={r.stderr.strip()[:600]!r} '
                  f'stdout-after={after.strip()[:200]!r}')
    failed += passed != len(cases)
    print(f'live-autostart {label}: {passed}/{len(cases)} passed')


def nm(obj):
    r = subprocess.run(['nm', str(obj)], capture_output=True, text=True, check=True)
    return {(ln.split()[-2], ln.split()[-1]) for ln in r.stdout.splitlines() if len(ln.split()) >= 2}


# The header: the request is an undefined reference to the anchor; the options macro defines the options object
# (C and C++, extern "C"); with PS3DK_PROF off neither leaves a symbol behind.
probe = out / 'header'
probe.mkdir()
(probe / 'request.c').write_text('#include <ps3dk/prof.h>\nint request_unit;\n')
(probe / 'options.c').write_text('#include <ps3dk/prof.h>\nPS3DK_PROF_LIVE_AUTOSTART_OPTIONS(4000, 256);\n')
(probe / 'options.cpp').write_text('#include <ps3dk/prof.h>\nPS3DK_PROF_LIVE_AUTOSTART_OPTIONS(4000, 0);\n')
hdr = []
for name, comp, defs, want_u, want_def in [
        ('request-on', cc, ['-DPS3DK_PROF=1', '-DPS3DK_PROF_LIVE_AUTOSTART=1'], True, False),
        ('request-unset', cc, ['-DPS3DK_PROF=1'], False, False),
        ('request-off', cc, ['-DPS3DK_PROF=0', '-DPS3DK_PROF_LIVE_AUTOSTART=1'], False, False),
        ('options-c', cc, ['-DPS3DK_PROF=1'], True, True),
        ('options-cxx', cxx, ['-DPS3DK_PROF=1'], True, True),
        ('options-off', cc, ['-DPS3DK_PROF=0'], False, False)]:
    source = probe / ('options.cpp' if name == 'options-cxx' else 'options.c' if name.startswith('options') else
                      'request.c')
    obj = probe / f'{name}.o'
    std = '-std=c++17' if comp == cxx else '-std=c11'
    r = subprocess.run([comp, std, '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic', *defs, '-I', str(inc.parent),
                        '-c', str(source), '-o', str(obj)], capture_output=True, text=True)
    if r.returncode:
        hdr.append(f'{name}: compile failed {r.stderr[-400:]}')
        continue
    syms = nm(obj)
    has_u = ('U', '__ps3dk_prof_live_autostart_anchor') in syms
    has_def = any(n == 'ps3dk_prof_live_autostart_settings' and t in 'RDBrdb' and t.isupper() for t, n in syms)
    if has_u != want_u or has_def != want_def:
        hdr.append(f'{name}: anchor-undefined={has_u} (want {want_u}) options-defined={has_def} (want {want_def})')
print(f'live-autostart header: {6 - len(hdr)}/6 passed')
for x in hdr:
    print('FAIL header', x)
failed += bool(hdr)

# The opt-in at link time: an archive of the runtime with the auto-start member; a program that requests it (the
# header define) extracts it and auto-starts; one that does not links no auto-start code and starts nothing.
link = out / 'link'
link.mkdir()
objs = []
for s in [*runtime, autostart, net / 'net_hold.c', net / 'net_pool.c']:
    o = link / f'{s.stem}.o'
    subprocess.run([cc, *base, '-c', str(s), '-o', str(o)], check=True)
    objs.append(str(o))
subprocess.run(['ar', 'rcs', str(link / 'libprof.a'), *objs], check=True)
(link / 'app.c').write_text(r'''#define _DEFAULT_SOURCE /* usleep under -std=c11 (GCC 14+: an implicit declaration is an error) */
#include <ps3dk/prof.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    ps3dk_prof_config c;
    (void)argc;
    ps3dk_prof_config_defaults(&c);
    c.app = "linkprobe";
    if (ps3dk_prof_set_sink_path(argv[1]) || ps3dk_prof_init(&c)) return 3;
    usleep(200000);
    return ps3dk_prof_shutdown();
}
''')
links = []
for name, defs, want in [('requested', ['-DPS3DK_PROF_LIVE_AUTOSTART=1'], True), ('not-requested', [], False)]:
    exe = link / f'app-{name}'
    r = subprocess.run([cc, '-std=c11', '-O2', '-no-pie', '-pthread', '-DPS3DK_PROF=1', *defs, '-I', str(inc.parent),
                        str(link / 'app.c'), '-L', str(link), '-lprof', '-Wl,-Map,' + str(exe) + '.map', '-o',
                        str(exe)], capture_output=True, text=True)
    if r.returncode:
        links.append(f'{name}: link failed {r.stderr[-400:]}')
        continue
    pulled = 'live_autostart.o' in (exe.with_suffix('.map') if False else Path(str(exe) + '.map')).read_text()
    d = link / f'run-{name}'
    d.mkdir()
    r = subprocess.run([str(exe), str(d)], capture_output=True, text=True, timeout=60)
    started = 'live viewer: connect to this console on port' in r.stderr
    if r.returncode or pulled != want or started != want:
        links.append(f'{name}: rc={r.returncode} member-pulled={pulled} started={started} (want {want}) '
                     f'{r.stderr[-300:]!r}')
print(f'live-autostart link: {2 - len(links)}/2 passed')
for x in links:
    print('FAIL link', x)
failed += bool(links)
raise SystemExit(1 if failed else 0)

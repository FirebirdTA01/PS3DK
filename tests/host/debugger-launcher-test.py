#!/usr/bin/env python3
"""Exercise launcher process/file boundaries without starting an emulator."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
CONFIG = 'Core:\n  PPU Decoder: Recompiler (LLVM)\nMiscellaneous:\n  GDB Server: 127.0.0.1:1\n  Automatically start games after boot: true\n  Start Paused: false\n'
EXPECTED = CONFIG.replace('Recompiler (LLVM)', 'Interpreter (static)').replace('127.0.0.1:1', '127.0.0.1:2345')

class LauncherTests(unittest.TestCase):
    def run_case(self, case, *, nogdb=False, existing=False, gdb_rc=0, emulator_rc=0, timeout=False, spawn=False, missing=False, old_time=False, stale=False, locked=False):
        d = OUT / (case + ' paths with spaces')
        d.mkdir(exist_ok=False)
        app = d / 'app'; app.mkdir()
        binary = app / 'app.fake.self'; binary.write_text('fixture')
        init = app / 'app.gdbinit'; init.write_text('fixture')
        marker = app / 'ps3dk-debug-wait'
        if existing: marker.write_text('owned by another invocation')
        if missing: binary.unlink()
        env = dict(os.environ, CASE_DIR=str(d), CASE_NO_GDB=str(int(nogdb)), CASE_GDB_RC=str(gdb_rc), CASE_EMULATOR_RC=str(emulator_rc),
                   CASE_TIMEOUT=str(int(timeout)), CASE_SPAWN=str(int(spawn)), CASE_OLD_TIME=str(int(old_time)), CASE_STALE=str(int(stale)), CASE_LOCKED=str(int(locked)))
        if os.name == 'nt':
            emulator = d / 'rpcs3.exe'; emulator.write_text('model')
            (d / 'config').mkdir(); cfg = d / 'config/config.yml'
            (d / 'log').mkdir()
            gdb = d / 'gdb.ps1'; gdb.write_text('exit ([int]$env:CASE_GDB_RC)\n')
            model = d / 'model.ps1'
            model.write_text(r'''
$ErrorActionPreference='Stop'
function Start-Process {
 param($FilePath,$ArgumentList,[switch]$PassThru,$WindowStyle)
 if($env:CASE_SPAWN -eq '1'){throw 'simulated spawn failure'}
 $cfg=$ArgumentList[[Array]::IndexOf($ArgumentList,'--config')+1].Trim('"')
 $obj=@{args=$ArgumentList;marker=(Test-Path -LiteralPath "$env:CASE_DIR/app/ps3dk-debug-wait");config=[IO.File]::ReadAllText($cfg);runConfig=$cfg;window=$WindowStyle}
 $obj|ConvertTo-Json -Depth 4|Set-Content "$env:CASE_DIR/observed.json"
 if($env:CASE_TIMEOUT -ne '1'){[IO.File]::WriteAllText("$env:CASE_DIR/log/RPCS3.log","config $(if($env:CASE_STALE -eq '1'){'prior-run.yml'}else{$cfg})`nGDB: Started listening on '127.0.0.1:2345'");if($env:CASE_OLD_TIME -eq '1'){(Get-Item "$env:CASE_DIR/log/RPCS3.log").LastWriteTime=(Get-Date).AddDays(-1)}}
 if($env:CASE_LOCKED -eq '1'){$script:lockedLog=New-Object IO.FileStream("$env:CASE_DIR/log/RPCS3.log",[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete),4096,[IO.FileOptions]::DeleteOnClose)}
 $p=[pscustomobject]@{HasExited=$false;Id=12345;ExitCode=[int]$env:CASE_EMULATOR_RC}
 $p|Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {$this.HasExited=$true}
 $p|Add-Member -MemberType ScriptMethod -Name Refresh -Value {if($env:CASE_NO_GDB -eq '1'){$this.HasExited=$true}}
 return $p
}
function Stop-Process {param($Id,$ErrorAction); [IO.File]::WriteAllText("$env:CASE_DIR/stopped.txt",[string]$Id)}
$argsList=@{Self="$env:CASE_DIR/app/app.fake.self";GdbInit="$env:CASE_DIR/app/app.gdbinit";Rpcs3="$env:CASE_DIR/rpcs3.exe";Gdb="$env:CASE_DIR/gdb.ps1";TimeoutSeconds=$(if($env:CASE_TIMEOUT -eq '1'){0}else{3})}
if($env:CASE_NO_GDB -eq '1'){$argsList.NoGdb=$true}
& $env:LAUNCHER @argsList
exit $LASTEXITCODE
''')
            env['LAUNCHER'] = str(ROOT / 'scripts/ps3dk-debug-rpcs3.ps1')
            argv = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(model)]
        else:
            cfgdir = d / 'rpcs3'; cfgdir.mkdir(); cfg = cfgdir / 'config.yml'
            env['XDG_CONFIG_HOME'] = str(d)
            emulator = d / 'rpcs3-model'
            emulator.write_text('''#!/usr/bin/env python3
import os,sys,json,time,signal
from pathlib import Path
d=Path(os.environ['CASE_DIR']);args=sys.argv[1:];cfg=Path(args[args.index('--config')+1])
def stopped(signum, frame):
 (d/'stopped.txt').write_text(str(os.getpid()))
 sys.exit(0)
signal.signal(signal.SIGTERM,stopped)
(d/'pid.txt').write_text(str(os.getpid()))
(d/'observed.json').write_text(json.dumps({'args':args,'marker':(d/'app/ps3dk-debug-wait').exists(),'config':cfg.read_text(),'runConfig':str(cfg)}))
if os.environ['CASE_TIMEOUT']!='1':
 log=d/'rpcs3/RPCS3.log'
 log.write_text('config '+('prior-run.yml' if os.environ['CASE_STALE']=='1' else str(cfg))+chr(10)+"GDB: Started listening on '127.0.0.1:2345'")
 if os.environ['CASE_OLD_TIME']=='1':os.utime(log,(1,1))
if os.environ['CASE_NO_GDB']=='1':
 time.sleep(.8)
 sys.exit(int(os.environ['CASE_EMULATOR_RC']))
else:time.sleep(10)
''')
            emulator.chmod(0o755)
            if spawn: emulator.write_text('#!/missing/interpreter\n')
            gdb = d / 'gdb-model'; gdb.write_text('#!/bin/sh\nexit "$CASE_GDB_RC"\n'); gdb.chmod(0o755)
            argv = ['sh', str(ROOT / 'scripts/ps3dk-debug-rpcs3.sh'), '--self', str(binary), '--gdbinit', str(init),
                    '--rpcs3', str(emulator), '--gdb', str(gdb), '--timeout', '0' if timeout else '3']
            if nogdb: argv.append('--no-gdb')
        cfg.write_bytes(CONFIG.encode())
        r = subprocess.run(argv, env=env, capture_output=True, timeout=15)
        (d / 'stdout.txt').write_bytes(r.stdout); (d / 'stderr.txt').write_bytes(r.stderr)
        (d / 'argv.json').write_text(json.dumps(argv, indent=2)); (d / 'rc.txt').write_text(str(r.returncode))
        self.assertEqual(cfg.read_bytes(), CONFIG.encode(), 'shared configuration changed')
        if existing:
            self.assertEqual(marker.read_text(), 'owned by another invocation')
            self.assertEqual(r.returncode, 2)
            message = (r.stdout + r.stderr).decode(errors='replace')
            self.assertIn(str(marker), message)
            self.assertIn('no debug session is running', message)
            self.assertFalse((d / 'observed.json').exists(), 'emulator spawned despite owned marker')
        else:
            self.assertFalse(marker.exists(), 'launcher leaked wait marker')
        observed = json.loads((d / 'observed.json').read_text(encoding='utf-8-sig')) if (d / 'observed.json').exists() else None
        if observed:
            self.assertIn('--no-gui', observed['args'])
            self.assertTrue(observed['marker'], 'marker absent at spawn')
            self.assertEqual(observed['config'].replace('\r\n', '\n'), EXPECTED)
            self.assertFalse(Path(observed['runConfig']).exists(), 'temporary config leaked')
            if os.name == 'nt': self.assertEqual(observed['window'], 'Hidden')
            must_stop = not nogdb or timeout or stale
            if must_stop:
                self.assertTrue((d / 'stopped.txt').exists(), 'owned emulator was not terminated')
                expected_pid = '12345' if os.name == 'nt' else (d / 'pid.txt').read_text()
                self.assertEqual((d / 'stopped.txt').read_text(), expected_pid, 'wrong process terminated')
            else:
                self.assertFalse((d / 'stopped.txt').exists(), 'terminated naturally exited emulator')
        else:
            self.assertFalse((d / 'stopped.txt').exists(), 'terminated process before owning one')
        return r, observed

    def test_success(self):
        r, observed = self.run_case('success'); self.assertEqual(r.returncode, 0); self.assertIsNotNone(observed)
    @unittest.skipUnless(os.name == 'nt', 'Windows handle-sharing contract')
    def test_open_log_delete_access(self):
        r, _ = self.run_case('delete-access', locked=True); self.assertEqual(r.returncode, 0)
    def test_open_log_old_timestamp(self):
        r, _ = self.run_case('old-timestamp', old_time=True); self.assertEqual(r.returncode, 0)
    def test_stale_log_refused(self):
        r, _ = self.run_case('stale-log', stale=True); self.assertEqual(r.returncode, 2)
    def test_external_gdb(self):
        r, observed = self.run_case('external', nogdb=True); self.assertEqual(r.returncode, 0); self.assertIsNotNone(observed)
        self.assertIn('Ctrl-C', r.stdout.decode(errors='replace'))
    def test_external_gdb_failure(self):
        r, _ = self.run_case('external-failure', nogdb=True, emulator_rc=7); self.assertEqual(r.returncode, 7)
    def test_gdb_failure(self):
        r, _ = self.run_case('gdb-failure', gdb_rc=7); self.assertEqual(r.returncode, 7)
    def test_readiness_timeout(self):
        r, _ = self.run_case('timeout', timeout=True, nogdb=True); self.assertEqual(r.returncode, 2)
    def test_spawn_failure(self):
        r, _ = self.run_case('spawn', spawn=True); self.assertEqual(r.returncode, 2)
    def test_existing_marker(self): self.run_case('existing', existing=True)
    def test_missing_self(self):
        r, observed = self.run_case('missing', missing=True); self.assertEqual(r.returncode, 2); self.assertIsNone(observed)

if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LauncherTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    totals = {'tests': result.testsRun, 'pass': result.testsRun-len(result.failures)-len(result.errors)-len(result.skipped),
              'fail': len(result.failures)+len(result.errors), 'skip': len(result.skipped)}
    (OUT / 'RESULT.json').write_text(json.dumps(totals, indent=2)+'\n')
    print(json.dumps(totals)); sys.exit(not result.wasSuccessful())

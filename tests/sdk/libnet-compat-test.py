#!/usr/bin/env python3
"""Linker-symbol compatibility inventory from the retired PSL1GHT libnet API.
Internal trampolines are excluded; h_errno remains a public data symbol.
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--ps3dev',type=Path,required=True)
p.add_argument('--archive',type=Path,help='check a controlled archive instead of the installed candidates')
p.add_argument('--red-control',action='store_true',help='remove netSocket in a temporary copy and require a failing child run')
a=p.parse_args()
if a.red_control:
 with tempfile.TemporaryDirectory(prefix='ps3dk-net-compat-') as temporary:
  mutant=Path(temporary)/'libnet.a'
  subprocess.run([str(a.ps3dev/'ppu/bin/powerpc64-ps3-elf-objcopy'),
                  '--strip-symbol=netSocket',str(a.ps3dev/'ps3dk/ppu/lib/libnet.a'),str(mutant)],check=True)
  red=subprocess.run([sys.executable,__file__,'--ps3dev',str(a.ps3dev),'--archive',str(mutant)],capture_output=True,text=True)
  print(red.stdout,end='')
  if red.returncode != 1 or 'netSocket' not in red.stdout:
   raise SystemExit('compatibility negative control did not fail as expected: '+red.stderr)
  print('missing-netSocket archive negative control: expected exit 1 PASS')
expected=set("""h_errno netAbortResolver netAbortSocket netAccept netBind netClose netCloseDump
netConnect netDeinitialize netErrnoLoc netFinalizeNetwork netFreethreadContext
netGetHostByAddr netGetHostByName netGetLibNameServer netGetNetEmuTestParam
netGetPeerName netGetSockInfo netGetSockInfoEx netGetSockName netGetSockOpt
netGetTestParam netGetUdpp2pTestparam netHErrnoLoc netIfCtl netInitialize
netInitializeNetworkEx netListen netOpenDump netPoll netReadDump netRecv
netRecvFrom netRecvMsg netSelect netSend netSendMsg netSendTo
netSetResolverConfigurations netSetSockOpt netSetTestParam netSetUdpp2pTestParam
netSetlibNameServer netSetnetemutestparam netShowIfConfig netShowNameserver
netShowNameServer netShowRoute netShutdown netSocket""".split())
failed=False
for abi,sub in [('ilp32',''),('lp64','lp64')]:
 lib=a.archive or a.ps3dev/'ps3dk/ppu/lib'/sub/'libnet.a'
 output=subprocess.check_output([str(a.ps3dev/'ppu/bin/powerpc64-ps3-elf-nm'),'-g','--defined-only',str(lib)],text=True)
 symbols={line.split()[-1] for line in output.splitlines() if len(line.split())==3}
 missing=sorted(expected-symbols)
 print(abi, 'legacy symbols:', 'PASS' if not missing else 'FAIL '+', '.join(missing))
 failed |= bool(missing)
raise SystemExit(failed)

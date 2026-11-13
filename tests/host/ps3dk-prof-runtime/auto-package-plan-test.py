#!/usr/bin/env python3
"""Compare default make plans and inspect explicit AUTO recipes; no target build."""
import argparse,io,json,shutil,subprocess,tarfile
from pathlib import Path

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
# The second PR's base: the default (non-AUTO) make plans must be unchanged from it.
ap.add_argument('--base',default='1218c61d2a1c20770e6ee477c038530ab38d9156')
ap.add_argument('--git',default='git',help='Git executable compatible with this worktree metadata')
a=ap.parse_args();root=Path(__file__).resolve().parents[3]
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
trees=('sdk/libgcm_cmd','sdk/libps3dkprof')
base=out/'base';base.mkdir();current=out/'current';current.mkdir()
raw=subprocess.check_output([a.git,'archive',a.base,*trees],cwd=root)
with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
    archive.extractall(base,filter='data')
files=subprocess.check_output([a.git,'ls-files','-co','--exclude-standard','-z',*trees],cwd=root)
for name in set(files.decode().split('\0'))- {''}:
    src=root/name
    if src.is_file():
        dst=current/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
records=[]
def plan(tree,component,goal):
    argv=['make','--no-print-directory','-Bn',goal,'PS3DEV=/sdk','PS3DK=/sdk',
          'PS3_BUILD_ROOT=/build','OBJ_DIR=/build/gcm','SDK_VERSION=0.0.0',
          'FIFO_SOURCE_COMMIT='+'1'*40,'AUTO_SOURCE_COMMIT='+'1'*40]
    r=subprocess.run(argv,cwd=tree/component,capture_output=True,timeout=60)
    name=tree.name+'-'+Path(component).name+'-'+goal
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    records.append(dict(name=name,argv=argv,rc=r.returncode))
    (out/'commands.json').write_bytes((json.dumps(records,indent=2)+'\n').encode())
    assert r.returncode==0,(name,r.stderr.decode())
    return r.stdout.decode()

for component in trees:
    before=plan(base,component,'all');after=plan(current,component,'all')
    assert before==after,component+' default plan changed'
    print(component+': default make plan byte-identical')

gcm=plan(current,trees[0],'auto')
compile_lines=[line for line in gcm.splitlines() if ' -c ' in line and '-DPS3TC_PROF_AUTO=1' in line]
assert len(compile_lines)==8,'four observed GCM objects per ABI'
for name in ('ps3tc_fifo_wrap.c','gcm_segment.c','rsx_legacy/commands.c','rsx_legacy/buffer.c'):
    lines=[line for line in compile_lines if name in line]
    assert len(lines)==2 and sum('-mlp64' in line for line in lines)==1,name
    assert all('-DPS3TC_FIFO_OBSERVE=1' in line for line in lines)
assert 'libgcm_cmd_prof_auto.a' in gcm
prof=plan(current,trees[1],'auto-rsx')
compile_lines=[line for line in prof.splitlines() if ' -c ' in line]
required={'auto_runtime','auto_startup','auto_config','auto_wire','auto_writer','auto_queue',
          'auto_commands','auto_completion','auto_frames','auto_clock','fifo_runtime','fifo_writer'}
for member in required:
    lines=[line for line in compile_lines if '/'+member+'.c' in line]
    assert len(lines)==2 and sum('-mlp64' in line for line in lines)==1,member
assert all('-DPS3DK_PROF_WITH_AUTO=1' in line and '-DPS3DK_PROF_WITH_FIFO=1' in line
           and '-fno-instrument-functions' in line for line in compile_lines)
assert prof.count('archive_digest.py verify')==2
assert 'libps3dkprof_auto.a' in prof and '--id "AUTO-RSX"' in prof
gcm_install=plan(current,trees[0],'install-auto')
prof_install=plan(current,trees[1],'install-auto-package')
for text in (gcm_install,prof_install):
    writes=[line for line in text.splitlines() if line.startswith('install -m')]
    assert not any('/libgcm_cmd.a' in line or '/libps3dkprof.a' in line or '/libps3prof.a' in line for line in writes)
for item in ('prof_auto.h','prof_fifo.h','prof.h','ps3-prof-auto.cmake','ps3-prof-auto-check.py',
             'ps3-prof-auto-identity.py'):
    assert item in prof_install,item
assert '--seal' in prof_install and '--source-commit' in prof_install
print('AUTO recipes: both ABIs, complete members, independent directories and explicit install PASS')

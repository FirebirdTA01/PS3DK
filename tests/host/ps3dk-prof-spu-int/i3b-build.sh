#!/usr/bin/env bash
# I3b host harness build: i3b-build.sh OUTDIR SRCDIR OLDDIR  (SRCDIR: sdk/libps3dkprof/src or a mutated copy;
# OLDDIR: the committed psp_ppu.c/.h, the table-identity reference; binaries OUTDIR/i3b-{plain,asan})
W=$(cd "$(dirname "$0")/../../.." && pwd); S=${2:-$W/sdk/libps3dkprof/src}; T=$W/tests/host/ps3dk-prof-spu-int
O=$1; mkdir -p $O/inc/ps3dk || exit 2; cp $W/sdk/include/ps3dk/prof.h $O/inc/ps3dk/
F="-std=c11 -O1 -g -pthread -Wall -Wextra -Werror -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS -DPS3DK_PROF_WITH_SPU -I$O/inc -I$S/core -I$S/pal -I$S/spu -I$S/rsx"
rc=0
for v in plain asan; do
  X=; [ $v = asan ] && X="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
  # the committed psp_ppu.c, every defined global renamed old_* (the table-identity reference)
  cc -std=c11 -O1 -g -Wall -Wextra $X -I$3 -I$S/spu -c $3/psp_ppu.c -o $O/old-$v.o || rc=1
  nm --defined-only -g $O/old-$v.o > $O/old-$v.nm || rc=1      # nm's own rc, not hidden by the pipe
  awk '{print $3" old_"$3}' $O/old-$v.nm > $O/old-$v.syms || rc=1
  [ -s $O/old-$v.syms ] || rc=1
  objcopy --redefine-syms=$O/old-$v.syms $O/old-$v.o $O/old-ren-$v.o || rc=1
  cc $F $X $S/core/prof_core.c $S/core/prof_drain.c $S/pal/pal_host.c $S/spu/psp_ppu.c $S/spu/psp_spu.c $S/spu/ext_runtime.c $S/spu/psp_elf.c $S/spu/psp_host_emul.c $S/rsx/gpu_runtime.c $S/rsx/prg.c $S/rsx/gpu_port_gcm.c $T/i3b_host.c $O/old-ren-$v.o -o $O/i3b-$v || rc=1
done
echo "i3b build rc=$rc"; exit $rc

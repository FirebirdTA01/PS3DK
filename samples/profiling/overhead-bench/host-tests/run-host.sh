#!/usr/bin/env bash
# overhead-bench A1 host controls (Linux/WSL host compiler) into OUT (a new directory).  usage: run-host.sh OUT
# Every step writes <step>.log and <step>.rc; SUMMARY.txt lists each step's rc and the totals it printed.  Exit 0
# only when every step's rc matched its expectation.
set -u
O=$1
mkdir "$O" || exit 3
H=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$H/.." && pwd)
W=$(cd "$B/../../.." && pwd)
CORE=$W/sdk/libps3dkprof/src/core
PAL=$W/sdk/libps3dkprof/src/pal
SPU=$W/sdk/libps3dkprof/src/spu
MODEL=$W/tests/host/ps3dk-prof-spu
HW=$W/samples/profiling/heterogeneous-workload
CC=${CC:-cc}
BASE="-std=c11 -O1 -g -Wall -Wextra -Werror"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
bad=0
: > "$O/SUMMARY.txt"
step() { # name expected-rc command...
    local name=$1 want=$2
    shift 2
    "$@" > "$O/$name.log" 2>&1
    local rc=$?
    echo "$rc" > "$O/$name.rc"
    local tot
    tot=$(grep -E '^(test_ob[a-z0-9_]*|ob_verify[a-z_]*|  ledger |  faulted|GPURT )' "$O/$name.log" | tr '\n' ' ')
    echo "$name rc=$rc want=$want $tot" >> "$O/SUMMARY.txt"
    [ "$rc" -eq "$want" ] || bad=1
}
# the public header alone on the host include path (the SDK's libc wrappers target newlib)
mkdir -p "$O/inc/ps3dk" && cp "$W/sdk/include/ps3dk/prof.h" "$O/inc/ps3dk/prof.h"
PROFH="-I$O/inc -I$CORE -I$PAL -pthread -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS"
LIB="$CORE/prof_core.c $CORE/prof_drain.c $CORE/prof_version.c $PAL/pal_host.c"
ppu_build() { # out sanitizer-flags
    local objs=()
    for s in $LIB; do $CC $BASE $2 $PROFH -DPS3DK_PROF=1 -c "$s" -o "$1.$(basename "$s").o" || return 1; objs+=("$1.$(basename "$s").o"); done
    $CC $BASE $2 $PROFH -DPS3DK_PROF=1 '-DOB_DEPTH_PROBE()=ob_depth_probe()' -include "$H/ob_depth_probe.h" \
        -c "$B/source/bench_ppu_on.c" -o "$1.on.o" || return 1
    $CC $BASE $2 $PROFH -DPS3DK_PROF=0 -c "$B/source/bench_ppu_off.c" -o "$1.off.o" || return 1
    $CC $BASE $2 $PROFH -DPS3DK_PROF=1 "$B/common/ob_cases.c" "$B/common/ob_sched.c" "$B/common/ob_csv.c" \
        "$B/common/ob_io.c" "$H/test_ob_ppu.c" \
        "$1.on.o" "$1.off.o" "${objs[@]}" -o "$1" || return 1
}
step build-ob-ppu 0 ppu_build "$O/test_ob_ppu" ""
step build-ob-ppu-san 0 ppu_build "$O/test_ob_ppu-san" "$SAN"
mkdir "$O/sink" "$O/sink-san"
step run-ob-ppu 0 "$O/test_ob_ppu" "$O/sink" "$O/ob-host-ppu.csv"
step run-ob-ppu-san 0 "$O/test_ob_ppu-san" "$O/sink-san" "$O/ob-host-ppu-san.csv"
SPUSRC="$SPU/psp_spu.c $SPU/psp_ppu.c $MODEL/model.c"
step build-ob-spu 0 $CC $BASE $SAN -I"$SPU" -I"$MODEL" $SPUSRC "$B/common/ob_cases.c" "$H/test_ob_spu.c" -o "$O/test_ob_spu"
step run-ob-spu 0 "$O/test_ob_spu"
step build-ob-units 0 $CC $BASE $SAN "$B/common/ob_sched.c" "$B/common/ob_cases.c" "$B/common/ob_teardown.c" \
    "$B/common/ob_rsx.c" "$H/test_ob_units.c" -o "$O/test_ob_units"
step run-ob-units 0 "$O/test_ob_units"
# the production driver, SPU lifecycle, GPU section and publication over fault-injecting mocks
LIFESRC="$B/common/ob_drv.c $B/common/ob_spu_life.c $B/common/ob_gpu_run.c $B/common/ob_io.c $B/common/ob_csv.c \
    $B/common/ob_cases.c $B/common/ob_sched.c $B/common/ob_rsx.c $B/common/ob_teardown.c"
step build-ob-life 0 $CC $BASE $SAN -pthread $LIFESRC "$H/test_ob_life.c" -o "$O/test_ob_life"
step run-ob-life 0 env OB_LIFE_CSV="$O/ob-life.csv" OB_LIFE_GPU_CSV="$O/ob-life.gpu.csv" "$O/test_ob_life"
# the GPU section on the REAL runtime and the REAL GCM port over fake device memory
RSX=$W/sdk/libps3dkprof/src/rsx
RTSRC="$CORE/prof_core.c $CORE/prof_drain.c $CORE/prof_version.c $PAL/pal_host.c $SPU/psp_ppu.c $SPU/psp_spu.c $SPU/ext_runtime.c \
    $SPU/psp_elf.c $SPU/psp_host_emul.c $RSX/gpu_runtime.c $RSX/prg.c $RSX/gpu_port_gcm.c $RSX/rsxcap.c"
GSRC="$B/common/ob_gpu_run.c $B/common/ob_rsx.c $B/common/ob_sched.c $B/common/ob_io.c $B/common/ob_teardown.c"
step build-ob-gpu-rt 0 $CC $BASE $SAN $PROFH -DPS3DK_PROF=1 -DPS3DK_PROF_WITH_SPU -I"$SPU" -I"$RSX" $RTSRC $GSRC \
    "$H/test_ob_gpu_rt.c" -o "$O/test_ob_gpu_rt"
mkdir "$O/gpu-rt"
for c in prod min under old; do
    mkdir "$O/gpu-rt/$c"
    step run-ob-gpu-rt-$c 0 "$O/test_ob_gpu_rt" "$c" "$O/gpu-rt/$c"
done
step py-verify 0 python3 -B -I "$H/test_ob_verify.py" --host-csv "$O/ob-host-ppu.csv" --life-csv "$O/ob-life.csv" \
    --life-gpu-csv "$O/ob-life.gpu.csv" --gpu-rt "$O/gpu-rt/prod/gpu-prod.csv" --gpu-rt "$O/gpu-rt/min/gpu-min.csv" --gpu-rt-neg "$O/gpu-rt/under/gpu-under.csv" --gpu-rt-neg "$O/gpu-rt/old/gpu-old.csv" \
    --out "$O/verify"
echo "overall=$([ $bad -eq 0 ] && echo PASS || echo FAIL)" >> "$O/SUMMARY.txt"
cat "$O/SUMMARY.txt"
exit $bad

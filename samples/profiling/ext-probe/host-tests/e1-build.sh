#!/usr/bin/env bash
# Build the E1 host probe (source/main.c + e1_host.c fakes + the real runtime, SPU core and writer) into OUT.
# usage: e1-build.sh OUT [plain|tsan]   (Linux/WSL host compiler; prints "build <s> rc=<n>")
set -u
O=$1; S=${2:-plain}
H=$(cd "$(dirname "$0")" && pwd); W=$(cd "$H/../../../.." && pwd)
L=$W/sdk/libps3dkprof/src
E=$W/samples/profiling/ext-probe
mkdir -p "$O/inc/ps3dk" && cp "$W/sdk/include/ps3dk/prof.h" "$O/inc/ps3dk/prof.h" || exit 3
SAN=""; EX=""
case "$S" in plain) ;; tsan) SAN="-fsanitize=thread" ;; tsan-neg) SAN="-fsanitize=thread"; EX="-DE1H_PLAIN_REPORT" ;;
  asan) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;; *) echo "unknown variant $S"; exit 2 ;; esac
WR=""
for f in ps3dk_prof_set_sink_path ps3dk_prof_init_ex ps3dk_prof_thread_begin ps3dk_prof_spu_image_register_table \
  ps3dk_prof_spu_lane_create ps3dk_prof_drain ps3dk_prof_spu_lane_get_stats ps3dk_prof_spu_lane_end \
  ps3dk_prof_gpu_attach ps3dk_prof_gpu_frame ps3dk_prof_gpu_begin ps3dk_prof_gpu_end ps3dk_prof_gpu_mark \
  ps3dk_prof_gpu_get_stats ps3dk_prof_gpu_detach ps3dk_prof_gpu_release ps3dk_prof_shutdown \
  ps3dk_prof_spu_attach ps3dk_prof_spu_flush ps3dk_prof_spu_dec_write ps3dk_prof_spu_fini; do WR="$WR -Wl,--wrap=$f"; done
cc -std=c11 -O1 -g -pthread -Wall -Wextra -Werror $SAN -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_TEST_HOOKS \
  -DPS3DK_PROF_WITH_SPU -DPS3DK_PROF_SPU_HOST_TEST=1 -DE1_HOST_TEST -DE1_BUILD_ID='"00e10570000beef0"' -DE1_WAIT_US=300000u -DE1_DESC_COUNT=6u $EX \
  -I$O/inc -I$L/core -I$L/pal -I$L/spu -I$L/rsx -I$E/source -I$E/host-tests -I$E/spu \
  $L/core/prof_core.c $L/core/prof_drain.c $L/core/prof_version.c $L/pal/pal_host.c $L/spu/psp_ppu.c $L/spu/psp_spu.c $L/spu/ext_runtime.c \
  $L/spu/psp_elf.c $L/spu/psp_host_emul.c $L/rsx/gpu_runtime.c $L/rsx/prg.c $L/rsx/gpu_port_gcm.c \
  $L/spu/prof_spu_writer.c $E/spu/e1_spu_core.c $E/host-tests/e1_host.c $E/source/main.c \
  $WR -o "$O/e1-$S" > "$O/build-$S.log" 2>&1
rc=$?; echo "build $S rc=$rc"; [ $rc -eq 0 ] || head -60 "$O/build-$S.log"; exit $rc

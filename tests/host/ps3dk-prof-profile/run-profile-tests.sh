#!/usr/bin/env bash
# PROFILE runtime controls (host): the digest tool controls, the host archive pipeline the Makefile target uses
# (members -> emit -> identity object -> final archive -> verify), the writer variants on the REAL runtime with the
# host PAL, and the reader's verdicts on the written captures.  usage: run-profile-tests.sh OUT_DIR READER
# OUT_DIR must not exist.  READER: a built ps3dk-prof binary.  Every step writes <step>.log/.rc; SUMMARY.txt; exit 0
# only when every step met its expectation.
set -u
O=$1
READER=$2
mkdir "$O" || exit 3
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
S=$R/sdk/libps3dkprof/src
CC=${CC:-cc}
bad=0
: > "$O/SUMMARY.txt"
step() { # name want command...
    local name=$1 want=$2
    shift 2
    "$@" > "$O/$name.log" 2>&1
    local rc=$?
    echo "$rc" > "$O/$name.rc"
    echo "$name rc=$rc want=$want $(grep -E '^(PW |digest_test|verified)' "$O/$name.log" | tr '\n' ' ')" >> "$O/SUMMARY.txt"
    [ "$rc" -eq "$want" ] || bad=1
}
mkdir -p "$O/inc/ps3dk" && cp "$R/sdk/include/ps3dk/prof.h" "$O/inc/ps3dk/prof.h"
FL="-std=c11 -O1 -g -pthread -Wall -Wextra -Werror -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_WITH_SPU -I$O/inc -I$S/core -I$S/pal -I$S/spu -I$S/rsx"
SRCS="core/prof_core.c core/prof_drain.c pal/pal_host.c spu/psp_ppu.c spu/psp_spu.c spu/ext_runtime.c spu/psp_elf.c spu/psp_host_emul.c rsx/gpu_runtime.c rsx/prg.c rsx/gpu_port_gcm.c rsx/rsxcap.c"
step digest-tool 0 python3 -I -B "$H/digest_test.py" "$O/digest" "$CC"
build_members() { # dir extra-flags
    mkdir -p "$1"
    for s in $SRCS; do
        $CC $FL $2 -c "$S/$s" -o "$1/$(basename "$s" .c).o" || return 1
    done
}
step build-members-profile 0 build_members "$O/obj-profile" "-DPS3DK_PROF_WITH_PROFILE"
step build-members-plain 0 build_members "$O/obj-plain" ""
pipeline() { # the Makefile ext-ilp32-profile recipe, with the host compiler and ar
    local D=$O/obj-profile
    rm -f "$D/members.a" "$D/final.a"
    ar rcsD "$D/members.a" "$D"/*.o || return 1
    python3 -I -B "$R/sdk/libps3dkprof/tools/archive_digest.py" emit "$D/members.a" --id RSX-HOST --premises 463 \
        --kinds 1 > "$D/prof_identity.c" || return 1
    $CC $FL -DPS3DK_PROF_WITH_PROFILE -c "$D/prof_identity.c" -o "$D/prof_identity.o" || return 1
    ar rcsD "$D/final.a" "$D"/prof_*.o "$D"/pal_host.o "$D"/psp_*.o "$D"/ext_runtime.o "$D"/gpu_*.o "$D"/prg.o \
        "$D"/rsxcap.o || return 1
    python3 -I -B "$R/sdk/libps3dkprof/tools/archive_digest.py" verify "$D/final.a" --id RSX-HOST --premises 463 --kinds 1
}
step archive-pipeline 0 pipeline
link() { # exe mode expect_hook expect_live stub-flags archive
    $CC $FL $( [ "$2" = profile ] && echo -DPS3DK_PROF_WITH_PROFILE ) -DEXPECT_HOOK="$3" -DEXPECT_LIVE="$4" \
        "$H/profile_writer.c" -c -o "$1.o" || return 1
    $CC $FL $5 -c "$H/hook_stub.c" -o "$1.stub.o" || return 1
    $CC -pthread "$1.o" "$1.stub.o" "$6" -o "$1"
}
step link-a 0 link "$O/pw-a" profile 0 0 "" "$O/obj-profile/final.a"
step link-b 0 link "$O/pw-b" profile 1 0 "-DSTUB_HOOK" "$O/obj-profile/final.a"
step link-c 0 link "$O/pw-c" profile 0 1 "-DSTUB_LIVE" "$O/obj-profile/final.a"
mkdir -p "$O/obj-plain" && ar rcsD "$O/obj-plain/plain.a" "$O"/obj-plain/*.o
step link-d 0 link "$O/pw-d" plain 0 0 "" "$O/obj-plain/plain.a"
for v in a b c d; do mkdir "$O/cap-$v"; done
step write-a 0 "$O/pw-a" "$O/cap-a" profile
step write-b 0 "$O/pw-b" "$O/cap-b" profile
step write-c 0 "$O/pw-c" "$O/cap-c" profile
step write-d 0 "$O/pw-d" "$O/cap-d" plain
CAP=$(ls "$O"/cap-a/*.prof 2>/dev/null | head -1)
step reader-check 0 "$READER" check "$CAP"
step reader-measured 0 "$READER" measured "$CAP"
step reader-report 0 "$READER" report "$CAP"
expect() { # log pattern
    grep -qF -- "$2" "$O/$1.log"
}
step measured-not-reviewed 0 expect reader-measured "profile classification RSX: NOT REVIEWED: profile not reviewed: RSX-HOST"
step measured-profile-line 0 expect reader-measured "profile: RSX-HOST (declared by the runtime)"
step report-profile-line 0 expect reader-report "SYS_SW_VERSION=NOT_READABLE HW_MODEL_CLASS=NOT_READABLE"
CAPD=$(ls "$O"/cap-d/*.prof 2>/dev/null | head -1)
step reader-plain-measured 0 "$READER" measured "$CAPD"
step plain-no-profile 0 expect reader-plain-measured "rsx mapping: no PROFILE record (file_flags bit 1 clear)"
# SPU order records on the REAL EXT capture path (spu-ticks slice delta D3): the runtime built with SPU lanes, PROFILE
# and the test hooks, the I2 host producers (real psp_spu core over the host line emulation), case basic-thread;
# the file must carry file_flags 7 and, per lane, LANE_CREATED first, at least one DRAINED+DRAIN_TB pair and
# ORDER_FINAL immediately before the lane's DOMAIN_FINAL (order_check.py), and the reader must accept it.
I2=$R/tests/host/ps3dk-prof-spu-int
WRAP=-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc
order_build() {
    local D=$O/obj-order
    build_members "$D" "-DPS3DK_PROF_WITH_PROFILE -DPS3DK_PROF_TEST_HOOKS" || return 1
    ar rcsD "$D/members.a" "$D"/*.o || return 1
    python3 -I -B "$R/sdk/libps3dkprof/tools/archive_digest.py" emit "$D/members.a" --id SPU-HOST --premises 1689         --kinds 2 > "$D/prof_identity.c" || return 1
    $CC $FL -DPS3DK_PROF_WITH_PROFILE -c "$D/prof_identity.c" -o "$D/prof_identity.o" || return 1
    $CC $FL -DPS3DK_PROF_WITH_PROFILE -DPS3DK_PROF_TEST_HOOKS -c "$I2/i2_test.c" -o "$D/i2_test.o" || return 1
    $CC -pthread "$D"/*.o -o "$O/i2-order" $WRAP
}
step build-order 0 order_build
mkdir "$O/cap-order"
step write-order 0 "$O/i2-order" basic-thread "$O/cap-order"
CAPO=$(ls "$O"/cap-order/*.prof 2>/dev/null | head -1)
step order-records 0 python3 -I -B "$H/order_check.py" "$CAPO"
step reader-order-check 0 "$READER" check "$CAPO"
step reader-order-ticks 0 "$READER" spu-ticks "$CAPO"
step order-ticks-records 0 expect reader-order-ticks "(file_flags bit 2)"
step order-ticks-final 1 expect reader-order-ticks "ORDER COVERAGE UNKNOWN"
echo "overall=$([ $bad -eq 0 ] && echo PASS || echo FAIL)" >> "$O/SUMMARY.txt"
cat "$O/SUMMARY.txt"
exit $bad

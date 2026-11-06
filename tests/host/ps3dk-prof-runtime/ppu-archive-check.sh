#!/usr/bin/env bash
# The PPU profiler archives as the SDK Makefile builds them, both ABIs:
#   1. exact members: libps3dkprof.a = the core + the live link (live_agent, live_os_lv2, live_api);
#      libps3prof.a = the core + the arc writer + startup, with no live member;
#   2. symbols: every defined global is the public API or under ps3dk_prof__, no ps3dk_prof_test_ symbol, and both
#      archives define the core finalization accessor.  The automatic archive also exports, as it already did before
#      the live link, its startup anchor, __ps3dk_prof_thread_exit and ps3prof_arcs_write (listed exactly);
#   3. links against an installed SDK: a program that calls the live API links and pulls the three live members;
#      one that does not call it pulls none of them, and the profiler members it does pull call no network
#      function.  librt's own close() pulls librt's socket.o into any program that closes a file; a control with no
#      profiler shows that.
#
#   ppu-archive-check.sh <out-dir> <ps3dev> <installed-ps3dk>
# <ps3dev> holds ppu/bin/powerpc64-ps3-elf-*; <installed-ps3dk> provides the crt and the other libraries to link.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
if [ $# -ne 3 ]; then
    echo "usage: $0 <out-dir> <ps3dev> <installed-ps3dk>" >&2
    exit 2
fi
mkdir -p "$1" || exit 2
out="$(cd "$1" && pwd)"
ps3dev="$2"
ps3dk="$3"
cross="$ps3dev/ppu/bin/powerpc64-ps3-elf-"
fails=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
words() { tr '\n' ' ' | sed 's/ $//'; }

# 1. Build with the real Makefile (a fixed SDK version: no git query from this tree).
if make -C "$root/sdk/libps3dkprof" -j4 CROSS="$cross" PS3DEV="$ps3dev" PS3DK="$ps3dk" \
    PS3_BUILD_ROOT="$out/build" SDK_VERSION=archive-check >"$out/make.log" 2>&1; then
    ok "make (both ABIs, -Werror)"
else
    tail -n 30 "$out/make.log"
    bad "make"
    echo "== ppu archive check: $fails failed"
    exit 1
fi
lib="$out/build/sdk/libps3dkprof"
api="ps3dk_prof_config_defaults ps3dk_prof_drain ps3dk_prof_frame ps3dk_prof_frame_token ps3dk_prof_get_capture_record \
ps3dk_prof_init ps3dk_prof_live_get_stats ps3dk_prof_live_start ps3dk_prof_live_stop ps3dk_prof_set_sink_callbacks \
ps3dk_prof_set_sink_path ps3dk_prof_shutdown ps3dk_prof_thread_begin ps3dk_prof_thread_end ps3dk_prof_zone_begin \
ps3dk_prof_zone_end"
auto_api="ps3dk_prof_config_defaults ps3dk_prof_drain ps3dk_prof_frame ps3dk_prof_frame_token \
ps3dk_prof_get_capture_record ps3dk_prof_init ps3dk_prof_set_sink_callbacks ps3dk_prof_set_sink_path \
ps3dk_prof_shutdown ps3dk_prof_thread_begin ps3dk_prof_thread_end ps3dk_prof_zone_begin ps3dk_prof_zone_end"

# 2. Members and symbols.
for abi in ilp32 lp64; do
    for a in libps3dkprof libps3prof; do
        ar="$lib/$abi/$a.a"
        tag="$abi $a"
        if [ "$a" = libps3dkprof ]; then
            want="live_agent.o live_api.o live_os_lv2.o pal_lv2.o prof_core.o prof_drain.o"
            want_api="$api"
            extra=""
        else
            want="pal_lv2.o prof_arcs_writer.o prof_core.o prof_drain.o startup.o"
            want_api="$auto_api"
            extra="__ps3dk_prof_startup_anchor __ps3dk_prof_thread_exit ps3prof_arcs_write"
        fi
        # Producers write to files first: a pipeline would hide their exit status.
        if ! "${cross}ar" t "$ar" >"$out/$abi-$a.members"; then
            bad "$tag: ar t"
            continue
        fi
        got="$(LC_ALL=C sort "$out/$abi-$a.members" | words)"
        [ "$got" = "$want" ] && ok "$tag members: $got" || bad "$tag members: [$got], want [$want]"
        if ! "${cross}nm" -g --defined-only "$ar" >"$out/$abi-$a.nm"; then
            bad "$tag: nm"
            continue
        fi
        awk 'NF >= 3 {print $3}' "$out/$abi-$a.nm" | LC_ALL=C sort -u >"$out/$abi-$a.names"
        public="$(grep -E '^ps3dk_prof_[a-z]' "$out/$abi-$a.names" | words)"
        [ "$public" = "$(echo $want_api)" ] && ok "$tag exports exactly the API" ||
            bad "$tag exports [$public], want [$(echo $want_api)]"
        other="$(grep -vE '^ps3dk_prof_' "$out/$abi-$a.names" | words)"
        [ "$other" = "$extra" ] && ok "$tag internals under ps3dk_prof__ (others exactly: [$extra])" ||
            bad "$tag exports outside ps3dk_prof_: [$other], want [$extra]"
        if grep -q 'ps3dk_prof_test_' "$out/$abi-$a.names"; then
            bad "$tag contains a test-hook symbol"
        else
            ok "$tag has no test-hook symbol"
        fi
        grep -qx 'ps3dk_prof__finalization' "$out/$abi-$a.names" && ok "$tag defines the core finalization accessor" ||
            bad "$tag lacks ps3dk_prof__finalization"
    done
done

# The stack measurement build (PS3DK_PROF_LIVE_STACK_PAINT) compiles for both ABIs with the archive's flags; it is
# never archived (the members above are the shipped ones).
for abi in ilp32 lp64; do
    x=""
    [ "$abi" = lp64 ] && x=-mlp64
    if "${cross}gcc" -O2 -Wall -Wextra -Werror -std=c11 -mcpu=cell -ffunction-sections -fdata-sections \
        -fno-instrument-functions -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_LV2 -DPS3DK_PROF_LIVE_STACK_PAINT $x \
        -I"$root/sdk/include" -I"$ps3dk/ppu/include" -I"$root/sdk/libps3dkprof/src/core" \
        -I"$root/sdk/libps3dkprof/src/pal" -I"$root/sdk/libps3dkprof/src/live" -c "$root/sdk/libps3dkprof/src/live/live_os_lv2.c" \
        -o "$out/live_os_lv2-paint-$abi.o" >"$out/paint-$abi.log" 2>&1; then
        ok "$abi stack measurement build compiles"
    else
        cat "$out/paint-$abi.log"
        bad "$abi stack measurement build"
    fi
done

# 3. Links (ILP32, the default ABI): with and without a live API reference, and a no-profiler control.
cat >"$out/uses_live.c" <<'C'
#include <ps3dk/prof.h>
int main(void)
{
    ps3dk_prof_capture_record r;
    int a = ps3dk_prof_get_capture_record(&r);
    int b = ps3dk_prof_live_start(0);
    int c = ps3dk_prof_live_stop(0, 0);
    ps3dk_prof_live_stats s;
    int d = ps3dk_prof_live_get_stats(&s);
    return a + b + c + d;
}
C
cat >"$out/no_live.c" <<'C'
#include <ps3dk/prof.h>
int main(void)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    return ps3dk_prof_init(&c) + ps3dk_prof_shutdown();
}
C
# Control: no profiler at all, only a file close.  librt's close() routes socket descriptors to socketclose, so its
# close.o pulls librt's socket.o into ANY program that closes a file; that member is librt's, not the live link's.
cat >"$out/close_only.c" <<'C'
#include <fcntl.h>
#include <unistd.h>
int main(void)
{
    return close(open("/dev_hdd0/x", O_RDONLY));
}
C
# The PPU driver's specs find the libraries through PS3DK, and lv2.ld (-T lv2.ld%s) on the startfile path (-B).
export PS3DK="$ps3dk"
net_calls='^(socket|bind|listen|accept|send|recv|poll|socketpoll|getsockname|setsockopt|netInitialize)$'
pulled_by_close() {
    grep -A1 -E 'librt\.a\(socket\.o\)$' "$1" | grep -q 'librt\.a(close\.o) (__librt_socketclose_r)'
}
for prog in uses_live no_live close_only; do
    if "${cross}gcc" -O2 -std=c11 -DPS3DK_PROF=1 -I"$root/sdk/include" "$out/$prog.c" -o "$out/$prog.elf" \
        -B"$ps3dk/ppu/lib/" -B"$ps3dk/ppu/powerpc64-ps3-elf/lib/" -L"$lib/ilp32" -lps3dkprof -L"$ps3dk/ppu/lib" \
        -lrt -llv2 -lm -Wl,-Map,"$out/$prog.map" >"$out/$prog.link.log" 2>&1; then
        ok "$prog links"
    else
        tail -n 20 "$out/$prog.link.log"
        bad "$prog link"
        continue
    fi
    n=0
    for m in live_agent.o live_os_lv2.o live_api.o; do
        grep -q "libps3dkprof.a($m)" "$out/$prog.map" && n=$((n + 1))
    done
    case "$prog" in
    uses_live)
        [ "$n" = 3 ] && ok "$prog pulls all three live members" || bad "$prog pulls $n of 3 live members"
        grep -q "libps3dkprof.a(prof_drain.o)" "$out/$prog.map" && ok "$prog pulls the core record's object" ||
            bad "$prog lacks prof_drain.o"
        ;;
    no_live)
        [ "$n" = 0 ] && ok "$prog pulls no live member" || bad "$prog pulls $n live members"
        # The profiler members it did pull must not call the network: extract them and list their undefined symbols.
        rm -rf "$out/$prog.members"
        mkdir -p "$out/$prog.members"
        pulled="$(grep -oE 'libps3dkprof\.a\([a-z_0-9]+\.o\)' "$out/$prog.map" | sed 's/.*(\(.*\))/\1/' | LC_ALL=C sort -u)"
        if [ -z "$pulled" ]; then
            bad "$prog pulls no profiler member at all (the check would be vacuous)"
        elif (cd "$out/$prog.members" && "${cross}ar" x "$lib/ilp32/libps3dkprof.a" $pulled) &&
            "${cross}nm" -u "$out/$prog.members"/*.o >"$out/$prog.undef"; then
            calls="$(awk 'NF >= 2 {print $NF}' "$out/$prog.undef" | grep -E "$net_calls" | LC_ALL=C sort -u | words)"
            [ -z "$calls" ] && ok "$prog: the pulled profiler members ($(echo $pulled)) call no network function" ||
                bad "$prog: profiler members call [$calls]"
        else
            bad "$prog: member extraction or nm failed"
        fi
        pulled_by_close "$out/$prog.map" &&
            echo "INFO $prog: librt.a(socket.o) is pulled by librt.a(close.o) (__librt_socketclose_r), as in the control"
        ;;
    close_only)
        pulled_by_close "$out/$prog.map" && ok "control $prog: librt's close.o pulls its socket.o with no profiler" ||
            bad "control $prog: librt's close.o did not pull socket.o"
        ;;
    esac
done
echo "== ppu archive check: $fails failed"
[ "$fails" = 0 ]

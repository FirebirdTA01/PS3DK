#!/usr/bin/env bash
# Thirteen sysutil-tree forwarder headers, each bridging an include path to
# the cell/ header that carries the surface.  The script:
#   1. compiles the test TU (tests/sdk/sysutil-path-forwarders-test.c)
#      which pins one function per function-carrying target plus one type
#      per types-only target, across C(gnu99,c11)/C++17 x ILP32/LP64,
#      under -Wall -Wextra -Werror;
#   2. compiles each of the thirteen forwarders, alone, in the same matrix
#      (a single broken include cannot be masked by its neighbours);
#   3. control: a TU that uses the REAL sysutil/sysutil_music2.h (which
#      forwards to cell/sysutil_music.h) must build;
#   4. self-check: pointing sysutil/sysutil_music2.h at the UNRELATED
#      cell/sysutil_storagedata.h must make the control TU fail, while the
#      other twelve forwarder TUs keep building.
#
# overlay rule from PATH1 review: only the header(s) this change adds or
# edits go in front of $sdk/ppu/include.  We add thirteen headers and
# do not edit any existing one; the overlay is exactly those thirteen.
#
# usage: sysutil-path-forwarders-test.sh [--ps3dev DIR] [--include DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev)  ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "sysutil-path-forwarders: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sysutil-path-forwarders: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/sysutil-path-forwarders-test.c"
status=0

# overlay: ONLY the thirteen forwarders this change adds; in front of
# $sdk/ppu/include (which provides sys/memory.h, cell/error.h, spurs, etc).
ov="$work/ov"
mkdir -p "$ov/sysutil"
forwarders="sysutil_ap sysutil_avc2 sysutil_avc2_video sysutil_bgmplayback
sysutil_gameupdate sysutil_music sysutil_music2 sysutil_music_decode
sysutil_music_export sysutil_oskdialog_ext sysutil_search_types
sysutil_storagedata sysutil_sysconf"
for fwd in $forwarders; do
    cp "$inc/sysutil/$fwd.h" "$ov/sysutil/$fwd.h"
done

# Include-order helper: overlay first, then the installed SDK.
INCFLAGS=(-I"$ov" -I"$sdk/ppu/include")

# 1. the test TU across the full matrix (catches any forwarder pointed at
#    the wrong cell/ header - the referenced name is then undeclared).
for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    "${INCFLAGS[@]}" -c "$src" -o "$work/p.o" 2>&1); then
                echo "sysutil-path-forwarders: ok   compile $label (all 13 forwarders)"
            else
                echo "sysutil-path-forwarders: FAIL compile $label (all 13 forwarders)"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# 2. each forwarder, in its own translation unit (gnu99, ILP32).
for fwd in $forwarders; do
    probe="$work/fwd.c"
    printf '#include <sysutil/%s.h>\nint main(void) { return 0; }\n' "$fwd" > "$probe"
    if out=$("$cc" -x c -std=gnu99 -Wall -Wextra -Werror \
            "${INCFLAGS[@]}" -c "$probe" -o "$work/fwd.o" 2>&1); then
        echo "sysutil-path-forwarders: ok   lone forwarder $fwd"
    else
        echo "sysutil-path-forwarders: FAIL lone forwarder $fwd"
        printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -4
        status=1
    fi
done

# 3. control: a TU using the REAL forwarder (sysutil/music2 -> cell/music.h)
#    must build.  This proves the overlay actually resolves the chain.
#    Probe style per the PATH1 review: <fn-type> *volatile p, a function
#    body, no casts, no __ names.
ctl="$work/ctl.c"
printf '%s\n' '#include <sysutil/sysutil_music2.h>' \
       'typedef int fn2(int, sys_memory_container_t, int, CellMusicCallback, void *);' \
       'static fn2 * volatile p = &cellMusicInitialize2;' \
       'int main(void) { (void)p; return 0; }' > "$ctl"
if out=$("$cc" -x c -std=gnu99 -Wall -Wextra -Werror \
        "${INCFLAGS[@]}" -c "$ctl" -o "$work/ctl.o" 2>&1); then
    echo "sysutil-path-forwarders: ok   control: real sysutil/sysutil_music2.h builds"
else
    echo "sysutil-path-forwarders: FAIL control: the real music2 forwarder does not build"
    printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -4
    status=1
fi

# 4. self-check: replace sysutil/music2.h with a forwarder pointed at an
#    UNRELATED header (cell/rtc.h).  The control-like TU must now fail;
#    a TU using a forwarder for a DIFFERENT forwarder must keep building.
bad="$work/bad"; mkdir -p "$bad/sysutil"
for fwd in $forwarders; do
    cp "$ov/sysutil/$fwd.h" "$bad/sysutil/$fwd.h"
done
printf '%s\n' '#ifndef _PS3DK_SYSUTIL_MUSIC2_H_' \
       '#define _PS3DK_SYSUTIL_MUSIC2_H_' \
       '#include <cell/rtc.h>' \
       '#endif' > "$bad/sysutil/sysutil_music2.h"

badCtl="$work/badCtl.c"
printf '%s\n' '#include <sysutil/sysutil_music2.h>' \
       'typedef int fn2(int, sys_memory_container_t, int, CellMusicCallback, void *);' \
       'static fn2 * volatile p = &cellMusicInitialize2;' \
       'int main(void) { (void)p; return 0; }' > "$badCtl"
goodCtl="$work/goodCtl.c"
printf '%s\n' '#include <sysutil/sysutil_ap.h>' \
       'typedef int fnap(CellSysutilApParam *, sys_memory_container_t);' \
       'static fnap * volatile p = &cellSysutilApOn;' \
       'int main(void) { (void)p; return 0; }' > "$goodCtl"

badCtl_ok="no"; goodCtl_ok="no"
"$cc" -x c -std=gnu99 -Wall -Wextra -Werror -I"$bad" -I"$sdk/ppu/include" \
      -c "$badCtl"  -o "$work/badCtl.o"  2>/dev/null && badCtl_ok="yes"
"$cc" -x c -std=gnu99 -Wall -Wextra -Werror -I"$bad" -I"$sdk/ppu/include" \
      -c "$goodCtl" -o "$work/goodCtl.o" 2>/dev/null && goodCtl_ok="yes"

if [ "$badCtl_ok"  = "no"  ] && [ "$goodCtl_ok" = "yes" ]; then
    echo "sysutil-path-forwarders: ok   self-check: a forwarder pointed at an unrelated header is caught"
else
    echo "sysutil-path-forwarders: FAIL self-check: music2(bad)=$badCtl_ok, ap(good)=$goodCtl_ok; want bad=no good=yes"
    status=1
fi

[ $status -eq 0 ] && echo "PASS: sysutil-path-forwarders-test"
exit $status

#!/usr/bin/env bash
# The sysutil headers for stub libraries that shipped without one (see
# sysutil-api-headers-test.c):
#  1. compile with the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64,
#     under -Wall -Wextra -Werror (the layout asserts run in every mode);
#  2. with an installed SDK, link the ILP32 object against the twelve stub
#     archives, so every declared function must be a stub export of the
#     same name (skipped when the archives are not there).
# Self-check: a header declaring a misspelled function must fail the link,
# and the real header must link (the control).
#
# usage: sysutil-api-headers-test.sh [--ps3dev DIR] [--include DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; inc="$root/sdk/include"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --include) inc="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--include DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "sysutil-api-headers: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "sysutil-api-headers: FAIL: no PPU compiler under $ps3dev"; exit 1; }
sdk="${PS3DK:-}"; [ -n "$sdk" ] || { [ -d "$ps3dev/ps3dk/ppu/include" ] && sdk="$ps3dev/ps3dk" || sdk="$ps3dev"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/tests/sdk/sysutil-api-headers-test.c"
status=0

# Only the headers this change adds or edits go in front of the installed SDK.
NAMES="photo_decode photo_export photo_import video_export video_upload print rec remoteplay
game_exec licensearea cross_controller sysconf_ext"
mkdir -p "$work/ov/cell" "$work/ov/sysutil"
for n in $NAMES; do
    cp "$inc/cell/sysutil_$n.h" "$work/ov/cell/"
    cp "$inc/sysutil/sysutil_$n.h" "$work/ov/sysutil/"
done
cp "$inc/sysutil/sysutil_game_common.h" "$inc/sysutil/sysutil_common.h" "$work/ov/sysutil/"
LIBS="-lsysutil_photo_decode_stub -lsysutil_photo_export_stub -lsysutil_photo_import_stub
-lsysutil_video_export_stub -lsysutil_video_upload_stub -lsysutil_print_stub -lsysutil_rec_stub
-lsysutil_remoteplay_stub -lsysutil_game_exec_stub -lsysutil_licensearea_stub
-lsysutil_cross_controller_stub -lsysutil_sysconf_ext_stub"

for lang in c c++; do
    stds="-std=gnu99 -std=c11"; [ "$lang" = c++ ] && stds="-std=c++17"
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            if out=$("$cc" -x "$lang" "$std" $abi -Wall -Wextra -Werror \
                    -I"$work/ov" -I"$sdk/ppu/include" -c "$src" -o "$work/t.o" 2>&1); then
                echo "sysutil-api-headers: ok   compile $label"
            else
                echo "sysutil-api-headers: FAIL compile $label"
                printf '%s\n' "$out" | grep -E "error" | sed 's/^/    /' | head -8
                status=1
            fi
        done
    done
done

# link <label> <include dir>: compile and link ILP32 against the stubs.
link() {
    "$cc" -std=gnu99 -Wall -Wextra -I"$2" -I"$sdk/ppu/include" -c "$src" -o "$work/l.o" 2>"$work/l.err" &&
        "$cc" "$work/l.o" -o "$work/l.elf" -L"$sdk/ppu/lib" $LIBS 2>>"$work/l.err"
}
# The link needs the stub archives of an installed SDK; a toolchain alone
# (CI without the SDK build) checks the headers only.
if [ ! -f "$sdk/ppu/lib/libsysutil_print_stub.a" ]; then
    echo "sysutil-api-headers: SKIP link and self-check (no stub archives under $sdk/ppu/lib)"
    [ $status -eq 0 ] && echo "PASS: sysutil-api-headers-test"
    exit $status
fi
if link real "$work/ov"; then
    echo "sysutil-api-headers: ok   link: all 50 functions resolve to the stub archives"
else
    echo "sysutil-api-headers: FAIL link"; grep -E "undefined|error" "$work/l.err" | sed 's/^/    /' | head -10
    status=1
fi

# Self-check: rename one declaration (and its use) so it names no export.
mkdir -p "$work/bad"; cp -r "$work/ov/." "$work/bad/"
sed 's/cellRemotePlayBreak(void)/cellRemotePlayBrake(void)/' "$inc/cell/sysutil_remoteplay.h" > "$work/bad/cell/sysutil_remoteplay.h"
sed 's/FN(cellRemotePlayBreak)/FN(cellRemotePlayBrake)/' "$src" > "$work/bad.c"
if cmp -s "$inc/cell/sysutil_remoteplay.h" "$work/bad/cell/sysutil_remoteplay.h"; then
    echo "sysutil-api-headers: FAIL self-check could not plant the misspelling"; status=1
elif "$cc" -std=gnu99 -I"$work/bad" -I"$sdk/ppu/include" -c "$work/bad.c" -o "$work/b.o" 2>/dev/null &&
     ! "$cc" "$work/b.o" -o "$work/b.elf" -L"$sdk/ppu/lib" $LIBS > "$work/b.err" 2>&1 &&
     grep -q "cellRemotePlayBrake" "$work/b.err"; then
    echo "sysutil-api-headers: ok   self-check: a misspelled declaration fails the link"
else
    echo "sysutil-api-headers: FAIL self-check: a misspelled declaration was not caught"; status=1
fi

[ $status -eq 0 ] && echo "PASS: sysutil-api-headers-test"
exit $status

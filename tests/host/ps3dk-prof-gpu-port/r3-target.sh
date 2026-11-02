#!/usr/bin/env bash
# P3.3 R3 target gate (ILP32 only): compile the GCM port, archive it, link the archive into a fixture that references
# every device op (under --gc-sections), and show each function retained, non-empty, with the expected access widths.
# Every tool rc is checked; the port object's undefined imports must equal EXPECT_U exactly.  Observed (try7): in this
# ILP32 ELFv1 image --gc-sections drops NO function of a pulled-in object (even an unreferenced one stays), so
# retention is OBJECT-granular: the negative control is a fixture referencing nothing of the port, whose image must
# hold none of it.  Nothing beyond "the port compiles, links and is retained for ILP32" follows; the access widths
# are OBSERVATIONS of this build, not A1/A2 evidence.
# usage: r3-target.sh OUT   (env: PS3DK = the installed SDK, holding ppu/bin, ppu/include and ppu/lib)
set -u
O=$1; mkdir "$O" || exit 2
W=$(cd "$(dirname "$0")/../../.." && pwd); S=$W/sdk/libps3dkprof/src/rsx; T=$W/tests/host/ps3dk-prof-gpu-port
B=$PS3DK/ppu/bin/powerpc64-ps3-elf-
# the GCM accessors behind the cellGcm inlines, memset (prepare/release), and the linker-defined TOC base
EXPECT_U="TOC. gcmGetLabelAddress gcmGetReportDataAddressLocation memset rsxFlushBuffer"
F="-std=c11 -O2 -Wall -Wextra -Werror -mcpu=cell -ffunction-sections -fdata-sections -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_LV2 -I$S -I$PS3DK/ppu/include"
rc=0
fail() { echo "FAIL: $*"; rc=1; }
step() { local log=$1; shift; "$@" > "$O/$log" 2>&1; local r=$?; echo "$log rc=$r"; [ $r = 0 ] || fail "$log"; return $r; }

step cc-port.txt "${B}gcc" $F -c "$S/gpu_port_gcm.c" -o "$O/gpu_port_gcm.o"
step cc-main.txt "${B}gcc" $F -c "$T/r3_link_main.c" -o "$O/r3_link_main.o"
step ar.txt "${B}ar" rcs "$O/libgpp.a" "$O/gpu_port_gcm.o"
step link.txt "${B}gcc" -mcpu=cell -Wl,--gc-sections "$O/r3_link_main.o" "$O/libgpp.a" -L"$PS3DK/ppu/lib" \
     -lgcm_cmd -lgcm_sys -lrsx -lio -lsysutil -lrt -llv2 -lm -o "$O/r3-link.elf"
step readelf-h.txt "${B}readelf" -h "$O/r3-link.elf"
grep -q "ELF64" "$O/readelf-h.txt" || fail "not ELF64"
step nm-u.txt "${B}nm" -u "$O/gpu_port_gcm.o"
got=$(awk '{print $NF}' "$O/nm-u.txt" | sed 's/^\.//' | sort -u | tr '\n' ' ' | sed 's/ $//')
echo "port undefined: [$got]"; [ "$got" = "$EXPECT_U" ] || fail "imports [$got] != [$EXPECT_U]"
step nm-elf.txt "${B}nm" -S --defined-only "$O/r3-link.elf"
step objdump.txt "${B}objdump" -d --no-show-raw-insn "$O/r3-link.elf"
# every port function in the linked image (ELFv1: the named symbol is its .opd descriptor, sized as the code; the
# descriptor holds the entry), non-empty, its code range disassembled from the linked image
for f in ps3dk_prof__gpp_prepare ps3dk_prof__gpp_release ps3dk_prof__gpp_bind gpp_reserve_pair gpp_flush gpp_read_label \
         gpp_write_label gpp_read_timer gpp_write_timer gpp_sync gpp_invoke.constprop.0 gpp_room gpp_gcm_report \
         gpp_gcm_label gpp_gcm_flush; do
  line=$(awk -v f="$f" '$4==f && ($3=="D" || $3=="d") {print $1" "$2}' "$O/nm-elf.txt")
  [ $(printf '%s\n' "$line" | grep -c .) = 1 ] || { fail "$f: descriptor count != 1"; continue; }
  set -- $line; d=$((16#$1)); sz=$((16#$2))
  "${B}objdump" -s -j .opd --start-address=$d --stop-address=$((d + 8)) "$O/r3-link.elf" > "$O/opd-$f.txt" 2>&1 \
    || { fail "$f: opd dump"; continue; }
  # ILP32 descriptors are {u32 entry, u32 toc} (8 bytes apart): the entry is the first word
  hex=$(awk '/^ [0-9a-f]+ /{print $2; exit}' "$O/opd-$f.txt")
  [ ${#hex} = 8 ] || { fail "$f: opd entry unreadable"; continue; }
  e=$((16#$hex))
  "${B}objdump" -d --no-show-raw-insn --start-address=$e --stop-address=$((e + sz)) "$O/r3-link.elf" > "$O/fn-$f.txt" 2>&1 \
    || { fail "$f: disassembly"; continue; }
  n=$(grep -Ec '^ +[0-9a-f]+:' "$O/fn-$f.txt")
  # the symbol range also holds the function's traceback table and alignment fill ("..."): retained = a non-empty
  # disassembly within the range containing a return (blr / conditional blr) or a tail branch
  echo "retained $f descriptor=0x$1 entry=$(printf '0x%x' $e) size=$sz words-shown=$n"
  { [ $sz -gt 0 ] && [ $n -gt 0 ] && [ $n -le $((sz / 4)) ] &&
    grep -Eq '^ +[0-9a-f]+:[[:space:]]+(b[a-z]*lr|b)([[:space:]]|$)' "$O/fn-$f.txt"; } || fail "$f not retained"
done
dz=$(awk '$4=="ps3dk_prof__gpp_dev_gcm" {print $2}' "$O/nm-elf.txt" | head -1)
echo "retained gpp_dev_gcm size=0x${dz:-none}"; [ -n "$dz" ] && [ $((16#$dz)) -gt 0 ] || fail "gpp_dev_gcm not retained"
# access-width observations
want() { grep -Eq "^ +[0-9a-f]+:[[:space:]]+$2([[:space:]]|\$)" "$O/fn-$1.txt" && echo "observed $1: $2" || fail "$1: no $2"; }
want gpp_read_timer ld; want gpp_write_timer std; want gpp_read_label lwz; want gpp_write_label stw
want gpp_sync hwsync # objdump's name for sync 0 (heavyweight sync)
grep -Eq "[[:space:]]bctrl" "$O/fn-gpp_invoke.constprop.0.txt" && echo "observed gpp_invoke: bctrl (a compiler-emitted indirect call)" \
  || fail "gpp_invoke: no bctrl"
# negative control: the same archive and libraries, a fixture referencing nothing of the port: its image must hold
# none of the port's symbols (the retention check above can see absence)
step cc-control.txt "${B}gcc" $F -DR3_LINK_CONTROL -c "$T/r3_link_main.c" -o "$O/r3_link_control.o"
step link-control.txt "${B}gcc" -mcpu=cell -Wl,--gc-sections "$O/r3_link_control.o" "$O/libgpp.a" \
     -L"$PS3DK/ppu/lib" -lgcm_cmd -lgcm_sys -lrsx -lio -lsysutil -lrt -llv2 -lm -o "$O/r3-control.elf"
if step nm-control.txt "${B}nm" -S --defined-only "$O/r3-control.elf"; then
  grep -q ' main$' "$O/nm-control.txt" || fail "control image unusable (no main)"
  k=$(grep -c 'gpp_' "$O/nm-control.txt")
  echo "control image: $k port symbols"; [ "$k" = 0 ] || fail "control image holds port symbols"
fi
echo "R3 TARGET $([ $rc = 0 ] && echo PASS || echo FAIL) rc=$rc"; exit $rc

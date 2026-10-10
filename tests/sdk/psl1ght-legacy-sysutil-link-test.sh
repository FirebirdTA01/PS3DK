#!/usr/bin/env bash
# PSL1GHT's sysutil names must LINK, not only compile.
#
# <sysutil/sysutil.h>, <sysutil/video.h> and <sysutil/msg.h> provide the
# PSL1GHT names (sysUtilRegisterCallback, videoGetState, msgDialogOpen2, ...)
# as static inline forwarders to the cell* functions.  Code that calls them
# without those headers - tiny3d does, and so does any prebuilt library
# compiled against PSL1GHT - needs real symbols, and libsysutil.a had none:
# AcidSampleV2 failed to link on sysUtilRegisterCallback / Unregister / Check
# from libtiny3d.a.  The stub archive now carries them as aliases of the
# cell* imports (same NID, same trampoline).
#
# This links one object that declares the legacy names itself (no SDK
# header), in both data models, with -lsysutil ALONE, as a PSL1GHT Makefile
# does: libsysutil.a is a linker script over the cell-named stub
# archives (sysutil, game, trophy), so every legacy name - sysutil, sysGame*,
# sysDisc*, sysTrophy* and PSL1GHT's *Ex import names - must resolve to the
# same import descriptor as its cell*/sceNpTrophy* counterpart.  Two more
# links per data model check the split-archive line (each _stub archive by
# name, no -lsysutil) and both lines together (each import descriptor once).
# Skips without PS3DEV.
#
# Usage: tests/sdk/psl1ght-legacy-sysutil-link-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "psl1ght-legacy-sysutil-link: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
nm="$ps3dev/ppu/bin/powerpc64-ps3-elf-nm"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "psl1ght-legacy-sysutil-link: FAIL: no PPU compiler under $ps3dev"; exit 1; }

# legacy name|cell name
pairs='sysUtilRegisterCallback|cellSysutilRegisterCallback
sysUtilCheckCallback|cellSysutilCheckCallback
sysUtilUnregisterCallback|cellSysutilUnregisterCallback
sysUtilGetSystemParamInt|cellSysutilGetSystemParamInt
sysUtilGetSystemParamString|cellSysutilGetSystemParamString
videoGetResolution|cellVideoOutGetResolution
videoConfigure|cellVideoOutConfigure
videoGetState|cellVideoOutGetState
videoGetDeviceInfo|cellVideoOutGetDeviceInfo
videoGetConfiguration|cellVideoOutGetConfiguration
videoGetResolutionAvailability|cellVideoOutGetResolutionAvailability
videoDebugSetMonitorType|cellVideoOutDebugSetMonitorType
videoRegisterCallback|cellVideoOutRegisterCallback
videoUnregisterCallback|cellVideoOutUnregisterCallback
videoGetNumberOfDevice|cellVideoOutGetNumberOfDevice
videoGetConvertCursorColorInfo|cellVideoOutGetConvertCursorColorInfo
msgDialogClose|cellMsgDialogClose
msgDialogOpenErrorCode|cellMsgDialogOpenErrorCode
msgDialogOpen|cellMsgDialogOpen
msgDialogProgressBarInc|cellMsgDialogProgressBarInc
msgDialogAbort|cellMsgDialogAbort
msgDialogOpen2|cellMsgDialogOpen2
msgDialogProgressBarReset|cellMsgDialogProgressBarReset
msgDialogProgressBarSetMsg|cellMsgDialogProgressBarSetMsg
oskGetInputText|cellOskDialogGetInputText
oskSetInitialInputDevice|cellOskDialogSetInitialInputDevice
oskGetSize|cellOskDialogGetSize
oskUnloadAsync|cellOskDialogUnloadAsync
oskDisableDimmer|cellOskDialogDisableDimmer
oskSetKeyLayoutOption|cellOskDialogSetKeyLayoutOption
oskAbort|cellOskDialogAbort
oskSetDeviceMask|cellOskDialogSetDeviceMask
oskSetSeparateWindowOption|cellOskDialogSetSeparateWindowOption
oskAddSupportLanguage|cellOskDialogAddSupportLanguage
oskLoadAsync|cellOskDialogLoadAsync
oskSetInitialKeyLayout|cellOskDialogSetInitialKeyLayout
oskSetLayoutMode|cellOskDialogSetLayoutMode
sysSaveListLoad2|cellSaveDataListLoad2
sysSaveListSave2|cellSaveDataListSave2
sysSaveListAutoLoad|cellSaveDataListAutoLoad
sysSaveListAutoSave|cellSaveDataListAutoSave
sysSaveFixedLoad2|cellSaveDataFixedLoad2
sysSaveFixedSave2|cellSaveDataFixedSave2
sysSaveAutoLoad2|cellSaveDataAutoLoad2
sysSaveAutoSave2|cellSaveDataAutoSave2
sysSaveDelete2|cellSaveDataDelete2
sysGameRegisterDiscChangeCallback|cellGameRegisterDiscChangeCallback
sysGameUnregisterDiscChangeCallback|cellGameUnregisterDiscChangeCallback
sysDiscRegisterDiscChangeCallback|cellDiscGameRegisterDiscChangeCallback
sysDiscUnregisterDiscChangeCallback|cellDiscGameUnregisterDiscChangeCallback
sysGameBootCheck|cellGameBootCheck
sysGameContentErrorDialog|cellGameContentErrorDialog
sysGameContentPermit|cellGameContentPermit
sysGameCreateGameData|cellGameCreateGameData
sysGameDataCheck|cellGameDataCheck
sysGameDeleteGameData|cellGameDeleteGameData
sysGameGetDiscContentInfoUpdatePath|cellGameGetDiscContentInfoUpdatePath
sysGameGetLocalWebContentPath|cellGameGetLocalWebContentPath
sysGameGetParamInt|cellGameGetParamInt
sysGameGetParamString|cellGameGetParamString
sysGameGetSizeKB|cellGameGetSizeKB
sysGamePatchCheck|cellGamePatchCheck
sysGameSetParamString|cellGameSetParamString
sysGameThemeInstall|cellGameThemeInstall
sysGameThemeInstallFromBuffer|cellGameThemeInstallFromBuffer
sysGameThemeInstallFromBufferEx|cellGameThemeInstallFromBuffer
sysGameRegisterDiscChangeCallbackEx|cellGameRegisterDiscChangeCallback
sysDiscRegisterDiscChangeCallbackEx|cellDiscGameRegisterDiscChangeCallback
sysTrophyInit|sceNpTrophyInit
sysTrophyAbortHandle|sceNpTrophyAbortHandle
sysTrophyCreateContext|sceNpTrophyCreateContext
sysTrophyCreateHandle|sceNpTrophyCreateHandle
sysTrophyDestroyContext|sceNpTrophyDestroyContext
sysTrophyDestroyHandle|sceNpTrophyDestroyHandle
sysTrophyGetGameIcon|sceNpTrophyGetGameIcon
sysTrophyGetGameInfo|sceNpTrophyGetGameInfo
sysTrophyGetGameProgress|sceNpTrophyGetGameProgress
sysTrophyGetRequiredDiskSpace|sceNpTrophyGetRequiredDiskSpace
sysTrophyGetTrophyIcon|sceNpTrophyGetTrophyIcon
sysTrophyGetTrophyInfo|sceNpTrophyGetTrophyInfo
sysTrophyGetTrophyUnlockState|sceNpTrophyGetTrophyUnlockState
sysTrophyRegisterContextEx|sceNpTrophyRegisterContext
sysTrophySetSoundLevel|sceNpTrophySetSoundLevel
sysTrophyTerm|sceNpTrophyTerm
sysTrophyUnlockTrophy|sceNpTrophyUnlockTrophy'

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
{
    echo '/* No SDK header: the legacy names are declared here, as a TU that never'
    echo '   includes <sysutil/*.h> (or a prebuilt PSL1GHT library) sees them. */'
    while IFS='|' read -r legacy cell; do
        echo "extern void $legacy(void); extern void $cell(void);"
    done <<< "$pairs"
    echo 'void (*volatile keep[])(void) = {'
    while IFS='|' read -r legacy cell; do echo "    $legacy, $cell,"; done <<< "$pairs"
    echo '};'
    echo 'int main(void) { return keep[0] == 0; }'
} > "$work/legacy.c"
{
    echo '/* The cell and sce names only, linked through the stub archives by name. */'
    cut -d'|' -f2 <<< "$pairs" | sort -u | while read -r cell; do
        echo "extern void $cell(void);"
    done
    echo 'void (*volatile keep[])(void) = {'
    cut -d'|' -f2 <<< "$pairs" | sort -u | while read -r cell; do echo "    $cell,"; done
    echo '};'
    echo 'int main(void) { return keep[0] == 0; }'
} > "$work/cell.c"

status=0
for abi in "" -mlp64; do
    label="${abi:-ilp32}"
    if ! out=$("$cc" $abi -O2 "$work/legacy.c" -lsysutil -o "$work/legacy.elf" 2>&1); then
        echo "psl1ght-legacy-sysutil-link: FAIL $label: link"
        printf '%s\n' "$out" | grep -E 'undefined reference|error' | head -n 30 | sed 's/^/    /'
        status=1; continue
    fi
    syms="$("$nm" "$work/legacy.elf")"
    # An alias is its own 8-byte .opd descriptor (compact OPD, both data
    # models) whose words equal the canonical one's: same import trampoline,
    # same TOC.  So compare the descriptor BYTES, not the symbol addresses.
    desc() {  # <hex address> -> the 8 descriptor bytes as hex
        local start=$((16#$1))
        "$objdump" -s --start-address=$start --stop-address=$((start + 8)) "$work/legacy.elf" \
            | awk '/^ [0-9a-f]+ /{print $2 $3}' | head -n 1
    }
    bad=0
    while IFS='|' read -r legacy cell; do
        a=$(printf '%s\n' "$syms" | awk -v s="$legacy" '$3==s{print $1; exit}')
        b=$(printf '%s\n' "$syms" | awk -v s="$cell" '$3==s{print $1; exit}')
        if [ -z "$a" ] || [ -z "$b" ]; then
            echo "psl1ght-legacy-sysutil-link: FAIL $label: $legacy=${a:-missing} $cell=${b:-missing}"
            bad=1; continue
        fi
        da=$(desc "$a"); db=$(desc "$b")
        if [ -z "$da" ] || [ "$da" != "$db" ]; then
            echo "psl1ght-legacy-sysutil-link: FAIL $label: $legacy descriptor ${da:-unreadable} != $cell descriptor ${db:-unreadable}"
            bad=1
        fi
    done <<< "$pairs"
    [ $bad -eq 0 ] && echo "psl1ght-legacy-sysutil-link: ok   $label: $(grep -c . <<< "$pairs") legacy names link to their cell* imports" || status=1

    # Split-archive link line: each _stub archive by name, no -lsysutil.
    refs="-lsysutil_stub -lsysutil_game_stub -lsysutil_np_trophy_stub"
    if out=$("$cc" $abi -O2 "$work/cell.c" $refs -o "$work/cell.elf" 2>&1); then
        echo "psl1ght-legacy-sysutil-link: ok   $label: split-archive link ($refs)"
    else
        echo "psl1ght-legacy-sysutil-link: FAIL $label: split-archive link"
        printf '%s\n' "$out" | grep -E 'undefined reference|error' | head -n 10 | sed 's/^/    /'
        status=1
    fi
    # Both lines together: every import descriptor still defined once.
    if out=$("$cc" $abi -O2 "$work/cell.c" -lsysutil $refs -o "$work/both.elf" 2>&1); then
        dup=$("$nm" "$work/both.elf" | awk '$2 ~ /^[DdTt]$/ {print $3}' | sort | uniq -d \
              | grep -xF -f <(cut -d'|' -f2 <<< "$pairs" | sort -u) || true)
        if [ -z "$dup" ]; then
            echo "psl1ght-legacy-sysutil-link: ok   $label: -lsysutil plus the _stub archives, each import once"
        else
            echo "psl1ght-legacy-sysutil-link: FAIL $label: defined more than once: $(echo $dup)"
            status=1
        fi
    else
        echo "psl1ght-legacy-sysutil-link: FAIL $label: -lsysutil plus the _stub archives"
        printf '%s\n' "$out" | grep -E 'multiple definition|undefined reference|error' | head -n 10 | sed 's/^/    /'
        status=1
    fi
done
[ $status -eq 0 ] && echo "psl1ght-legacy-sysutil-link: PASS"
exit $status

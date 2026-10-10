#!/usr/bin/env bash
# Codegen guard for the vertex-program literal upload (<cell/gcm/gcm_cg_bridge.h>).
#
# A vertex program's literal pool ("internal-constant-N", e.g. the 1.0 of
# float4(pos, 1.0) at c[467]) must reach the constant bank before a draw.
# cellGcmSetVertexProgram uploads it when it binds the program, and
# cellGcmCgUploadInternalConsts uploads it on its own, for code that binds
# some other way (cellGcmSetVertexProgramLoad, a custom bind) or that
# restores the literals after something overwrote them.  Each upload is one
# NV40 UPLOAD_CONST_ID packet per literal: method header
# (5 << 18) | 0x1efc = 0x141efc.
#
# Rows compile one small TU each, in both data models, at -O0, -O2 and -Os,
# and count the instructions that form the 0x1efc half of that header:
#   - bind:         cellGcmSetVertexProgram alone           -> must upload
#   - helper:       cellGcmCgUploadInternalConsts alone      -> must upload
#   - load_helper:  cellGcmSetVertexProgramLoad + the helper -> must upload
#   - control:      cellGcmSetVertexProgramLoad alone        -> must NOT
# The control keeps the count honest: SetVertexProgramLoad has no parameter
# table, so it cannot upload literals.  Red control: a no-op helper (the
# rejected v0.22.1 rc2, 48887d68) fails the helper and load_helper rows.
# Skips without PS3DEV.
#
# Usage: tests/sdk/gcm-vp-literal-upload-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -I<dir> to test a candidate header).
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "gcm-vp-literal-upload: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "gcm-vp-literal-upload: FAIL: no PPU compiler under $ps3dev"; exit 1; }

# The SDK headers: <root>/ps3dk/ppu/include in a staged build, <root>/ppu/include
# in an installed package.
inc="$ps3dev/ps3dk/ppu/include"
[ -f "$inc/cell/gcm.h" ] || inc="$ps3dev/ppu/include"
[ -f "$inc/cell/gcm.h" ] || { echo "gcm-vp-literal-upload: FAIL: no cell/gcm.h under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
# One translation unit per row, so the count covers the inline helpers
# whether or not the optimiser inlines them (at -O0 they stay out of line).
hdr='#include <cell/gcm.h>'
printf '%s\n%s\n' "$hdr" 'void f (CellGcmContextData *ctx, CGprogram p, void *u) { cellGcmSetVertexProgram (ctx, p, u); }' > "$work/bind.c"
printf '%s\n%s\n' "$hdr" 'void f (CellGcmContextData *ctx, CGprogram p) { cellGcmCgUploadInternalConsts (ctx, p); }' > "$work/helper.c"
printf '%s\n%s\n' "$hdr" 'void f (CellGcmContextData *ctx, const CellCgbVertexProgramConfiguration *c, const void *u, CGprogram p) { cellGcmSetVertexProgramLoad (c, u); cellGcmCgUploadInternalConsts (ctx, p); }' > "$work/load_helper.c"
printf '%s\n%s\n' "$hdr" 'void f (const CellCgbVertexProgramConfiguration *c, const void *u) { cellGcmSetVertexProgramLoad (c, u); }' > "$work/control.c"

# Instructions carrying 0x1efc (7932), the low half of the UPLOAD_CONST_ID
# header.  The other VP methods (0x1e9c load, 0x1ef8 timeout, 0x1ff0 attrib
# mask) differ in the low half, so they do not count.
count() { grep -cE '(,|[[:space:]])(7932|0x1efc)([^0-9a-fA-Fx]|$)' "$1"; }

status=0
for model in "" -mlp64; do
    for opt in -O0 -O2 -Os; do
        label="${model:-ILP32} $opt"
        line=""; ok=1
        for row in bind helper load_helper control; do
            if ! "$cc" ${EXTRA_CFLAGS:-} -mcpu=cell $model $opt -std=gnu11 -I"$inc" -S "$work/$row.c" -o "$work/$row.s" 2> "$work/err.txt"; then
                echo "gcm-vp-literal-upload: FAIL $label $row does not compile"; head -5 "$work/err.txt"; ok=0; continue
            fi
            n=$(count "$work/$row.s")
            line="$line $row=$n"
            if [ "$row" = control ]; then [ "$n" -eq 0 ] || ok=0; else [ "$n" -ge 1 ] || ok=0; fi
        done
        if [ $ok -eq 1 ]; then
            echo "gcm-vp-literal-upload: ok  $label$line"
        else
            echo "gcm-vp-literal-upload: FAIL $label$line (want bind, helper, load_helper >= 1 and control = 0)"
            status=1
        fi
    done
done
[ $status -eq 0 ] && echo "gcm-vp-literal-upload: PASS" || echo "gcm-vp-literal-upload: FAIL"
exit $status

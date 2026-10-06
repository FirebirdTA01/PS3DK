#!/usr/bin/env bash
# The Cell-named link-time GCM commands declared in
# <cell/gcm/gcm_command_link.h> (the rsx-backed set: 18 base commands and
# 119 *Unsafe variants) compile through <cell/gcm.h> and are defined in
# libgcm_cmd.a, in both data models.
#
# Rows:
#   header   the header declares exactly 137 cellGcm* prototypes, none of
#            them cellGcmFlushUnsafe / cellGcmFinishUnsafe (static inline in
#            gcm_command_c.h; their link-time form is deferred).
#   compile  a unit that includes <cell/gcm.h> and takes the address of every
#            name compiles under -Wall -Wextra -Werror: gnu99, c11 and c++17,
#            ILP32 and -mlp64.
#   archive  libgcm_cmd.a (ILP32, and lp64/) defines every name exactly once
#            (nm T or D), and defines neither Flush/FinishUnsafe.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: gcm-link-surface-test.sh [--ps3dev DIR] [--lib DIR] [--include DIR]...
#   --lib      directory holding libgcm_cmd.a and lp64/libgcm_cmd.a
#              (default: $PS3DK/ppu/lib, else $ps3dev/ps3dk/ppu/lib)
#   --include  header directories, in order (default: this tree's
#              sdk/include and sdk/libgcm_cmd/include)
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; lib=""; incs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --lib) lib="$2"; shift 2 ;;
        --include) incs+=("-I$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--lib DIR] [--include DIR]..." >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "gcm-link-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
[ ${#incs[@]} -gt 0 ] || incs=("-I$root/sdk/include" "-I$root/sdk/libgcm_cmd/include")
if [ -z "$lib" ]; then
    if [ -n "${PS3DK:-}" ]; then lib="$PS3DK/ppu/lib"; else lib="$ps3dev/ps3dk/ppu/lib"; fi
fi
bin="$ps3dev/ppu/bin/powerpc64-ps3-elf"
status=0
fail() { echo "gcm-link-surface: FAIL: $*"; status=1; }
ok() { echo "gcm-link-surface: ok   $*"; }
[ -x "$bin-gcc" ] || [ -x "$bin-gcc.exe" ] || { echo "gcm-link-surface: FAIL: no PPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT

# ---- header ------------------------------------------------------------------
hdr="$root/sdk/include/cell/gcm/gcm_command_link.h"
sed -n 's/^[A-Za-z_][A-Za-z0-9_ *]*[ *]\(cellGcm[A-Za-z0-9_]*\)(.*/\1/p' "$hdr" | tr -d '\r' > "$work/names"
n=$(wc -l < "$work/names")
dups=$(sort "$work/names" | uniq -d | head -3 | tr '\n' ' ')
if [ "$n" -ne 137 ]; then
    fail "header: $n prototypes, expected 137"
elif [ -n "$dups" ]; then
    fail "header: declared twice: $dups"
elif grep -qx -E 'cellGcm(Flush|Finish)Unsafe' "$work/names"; then
    fail "header: declares cellGcmFlushUnsafe/cellGcmFinishUnsafe"
else
    ok "header: 137 prototypes"
fi

# ---- compile --------------------------------------------------------------------
{
    echo '#include <cell/gcm.h>'
    echo 'void (*const gcm_link_surface[])(void) = {'
    sed 's/.*/    (void (*)(void))\&&,/' "$work/names"
    echo '};'
} > "$work/use.c"
for lang in c c++; do
    drv="$bin-gcc"; stds="-std=gnu99 -std=c11"
    [ "$lang" = c++ ] && { drv="$bin-g++"; stds="-std=c++17"; }
    for std in $stds; do
        for abi in "" -mlp64; do
            label="$lang $std ${abi:-ilp32}"
            row="$work/${lang}_${std#-std=}_${abi:-ilp32}"
            if "$drv" -x "$lang" "$std" $abi -Wall -Wextra -Werror "${incs[@]}" \
                    -c "$work/use.c" -o "$row.o" > "$row.log" 2>&1; then
                ok "compile $label"
            else
                fail "compile $label: $(grep -m1 -E 'error' "$row.log")"
            fi
        done
    done
done

# ---- archive -------------------------------------------------------------------
for a in "$lib/libgcm_cmd.a" "$lib/lp64/libgcm_cmd.a"; do
    if [ ! -f "$a" ]; then
        fail "archive: no $a"
        continue
    fi
    # A PPC64 function symbol names its .opd descriptor, so nm shows D (and T
    # for the code entry when the dot symbol is kept); either defines it.
    "$bin-nm" -g --defined-only "$a" 2>/dev/null | awk '$2 == "T" || $2 == "D" { print $3 }' | tr -d '\r' | sort > "$work/defined"
    missing=$(sort "$work/names" | comm -23 - "$work/defined" | head -5 | tr '\n' ' ')
    twice=$(uniq -d "$work/defined" | grep -x -F -f "$work/names" | head -3 | tr '\n' ' ')
    extra=$(grep -x -E 'cellGcm(Flush|Finish)Unsafe' "$work/defined" | tr '\n' ' ')
    if [ -n "$missing" ]; then
        fail "archive $a: not defined: $missing"
    elif [ -n "$twice" ]; then
        fail "archive $a: defined twice: $twice"
    elif [ -n "$extra" ]; then
        fail "archive $a: defines $extra"
    else
        ok "archive $a: all 137 defined once"
    fi
done

[ "$status" -eq 0 ] && echo "gcm-link-surface: PASS"
exit $status

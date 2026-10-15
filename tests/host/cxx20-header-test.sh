#!/bin/sh
# C++20 readiness probe for the PPU header set (t_40879841).
#
# For every header under sdk/include (the PPU tree, not sdk/include-spu),
# compile a one-line TU `#include <header>` under
#   -std=gnu++17 and -std=gnu++20
# for both ILP32 (the toolchain default) and LP64 (`-mlp64`).
#
# A header that fails under gnu++17 is not a C++20 regression -- it is
# already broken on the older dialect.  The probe's purpose is the
# reverse: it catches, in ILP32 and LP64 separately, a header that
# PASSES gnu++17 but FAILS gnu++20.  Each such cell is listed with the
# first error line, and the script exits 1.
#
# Exit:
#   0   no gnu++20 regressions, or the toolchain/headers are absent
#       (a SKIP line is printed in that case).
#   1   at least one header passes gnu++17 and fails gnu++20.
#
# POSIX sh.  No Python.

set -u

# --------------------------------------------------------------- locate
# Prefer $PS3DEV if it points at a working PPU toolchain; otherwise try
# the common install locations.  If nothing works, emit SKIP + exit 0.
PS3DEV="${PS3DEV:-}"
if [ -z "$PS3DEV" ] && [ -x "$HOME/ps3dev/ppu/bin/powerpc64-ps3-elf-g++" ]; then
    PS3DEV="$HOME/ps3dev"
fi

# Repo root = the directory containing sdk/include (walk up from this
# script's directory).  Only used when the installed SDK is absent.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P 2>/dev/null)"
while [ -n "$ROOT" ] && [ "$ROOT" != "/" ] && [ ! -d "$ROOT/sdk/include" ]; do
    ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd -P 2>/dev/null)" || ROOT=""
    [ -z "$ROOT" ] && break
done
HDR_SRC="$ROOT/sdk/include"

if [ -n "$PS3DEV" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++" ] && [ -d "$PS3DEV/ps3dk/ppu/include" ]; then
    GXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
    HDR="$PS3DEV/ps3dk/ppu/include"
elif [ -n "${PS3DK:-}" ] && [ -x "$PS3DK/ppu/bin/powerpc64-ps3-elf-g++" ] && [ -d "$PS3DK/ps3dk/ppu/include" ]; then
    GXX="$PS3DK/ppu/bin/powerpc64-ps3-elf-g++"
    HDR="$PS3DK/ps3dk/ppu/include"
elif command -v powerpc64-ps3-elf-g++ >/dev/null 2>&1 && [ -d "$HDR_SRC" ]; then
    GXX="powerpc64-ps3-elf-g++"     # rely on PATH
    HDR="$HDR_SRC"
else
    echo "SKIP cxx20-header-test: no PPU g++ reachable (need \$PS3DEV or powerpc64-ps3-elf-g++ on PATH, and ps3dk/ppu/include)"
    exit 0
fi

# ------------------------------------------------- enumerate the headers
# One header path per line, relative to HDR (matching the include form
# the TU uses), sorted for a deterministic table.
list_file=$(mktemp -t cxx20-list.XXXXXX)
find "$HDR" -type f -name '*.h' 2>/dev/null \
    | sed -e "s|^$HDR/||" \
    | sort > "$list_file"
n_headers=$(wc -l < "$list_file" 2>/dev/null); n_headers=${n_headers:-0}
if [ "$n_headers" -eq 0 ]; then
    echo "SKIP cxx20-header-test: no .h files under $HDR"
    rm -f "$list_file"
    exit 0
fi

# ------------------------------------------------------ compile probes
# probe <relative-header> <std> <extra-flags...>  -> 0 on success.
# The TU is read from stdin as a one-line `#include <...>` so we do not
# litter the tree with scratch files.
probe() {
    printf '#include <%s>\n' "$1" \
        | "$GXX" -fsyntax-only -isystem "$HDR" \
            ${3:+"$3"} "$2" -x c++ - >/dev/null 2>&1
}

# first_err <relative-header> <std> <extra-flags...>  -> first diagnostic
# line from the same compilation, or "<no first error line captured>".
first_err() {
    _fe=$(printf '#include <%s>\n' "$1" \
        | "$GXX" -fsyntax-only -isystem "$HDR" \
            ${3:+"$3"} "$2" -x c++ - 2>&1 \
        | grep -E -m1 'error:|Error')
    _fe=${_fe:-<no first error line captured>}
    printf '%s\n' "$_fe"
}

# ------------------------------------------------------- main loop
# We read from the list file (a redirection, not a pipe) so the shell
# does not fork into a subshell: counters accumulate across the loop.
REGR=0
TABLE=$(mktemp -t cxx20-table.XXXXXX)
REGRS=$(mktemp -t cxx20-regrs.XXXXXX)

printf '  %-5s %-5s %-6s  %s\n' \
    "17-il" "17-lp" "20-il/20-lp" "header" > "$TABLE"

while IFS= read -r rel; do
    [ -z "$rel" ] && continue

    p17i=0; p17l=0; p20i=0; p20l=0
    probe "$rel" "-std=gnu++17" ""        && p17i=1
    probe "$rel" "-std=gnu++17" "-mlp64"  && p17l=1
    probe "$rel" "-std=gnu++20" ""        && p20i=1
    probe "$rel" "-std=gnu++20" "-mlp64"  && p20l=1

    if [ "$p17i" -eq 1 ]; then a17i="PASS"; else a17i="FAIL"; fi
    if [ "$p17l" -eq 1 ]; then a17l="PASS"; else a17l="FAIL"; fi
    if [ "$p20i" -eq 1 ]; then a20i="PASS"; else a20i="FAIL"; fi
    if [ "$p20l" -eq 1 ]; then a20l="PASS"; else a20l="FAIL"; fi

    printf '  %-5s %-5s %-6s  %s\n' \
        "$a17i" "$a17l" "$a20i/$a20l" "$rel" >> "$TABLE"

    # A regression needs: passed gnu++17 in an ABI, failed gnu++20 in the
    # SAME ABI.  Report one line per failing cell, with the first error.
    if [ "$p17i" -eq 1 ] && [ "$p20i" -eq 0 ]; then
        REGR=$((REGR + 1))
        printf '  REGRESSION [ilp32] %s\n    %s\n' \
            "$rel" "$(first_err "$rel" -std=gnu++20 "")" >> "$REGRS"
    fi
    if [ "$p17l" -eq 1 ] && [ "$p20l" -eq 0 ]; then
        REGR=$((REGR + 1))
        printf '  REGRESSION [lp64 ] %s\n    %s\n' \
            "$rel" "$(first_err "$rel" -std=gnu++20 -mlp64)" >> "$REGRS"
    fi
done < "$list_file"
rm -f "$list_file"

# ------------------------------------------------------- report
{
    echo "cxx20-header-test"
    echo "  g++   : $GXX"
    echo "  hdr   : $HDR"
    echo "  count : $n_headers headers probed (ILP32 and LP64)"
    echo
    echo "  table (P = passes that standard, F = fails):"
    sed 's/^/    /' "$TABLE" | head -60
    if [ "$n_headers" -gt 60 ]; then
        echo "    ... (showing first 60 of $n_headers)"
    fi
    echo
    if [ "$REGR" -gt 0 ]; then
        echo "  C++20 regressions (passed gnu++17, failed gnu++20):"
        sed 's/^/    /' "$REGRS"
        echo
        echo "CXX20_HEADER_TEST_FAIL"
    else
        echo "CXX20_HEADER_TEST_OK"
    fi
}
rm -f "$TABLE" "$REGRS"

[ "$REGR" -gt 0 ] && exit 1
exit 0

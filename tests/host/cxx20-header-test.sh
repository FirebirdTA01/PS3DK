#!/bin/sh
# cxx20-header-test.sh — C++20 readiness probe for the PPU header set.
#
# For every header under sdk/include (PPU tree, NOT sdk/include-spu),
# compile a one-line TU `#include <header>` under:
#   -std=gnu++17  AND  -std=gnu++20
#   ILP32 (default)  AND  LP64 (-mlp64)
#
# Cell result:
#   PASS   g++ exits 0
#   FAIL   g++ exits 1  (a real compiler diagnostic; may be a C++20 regression)
#   INFRA  g++ exits anything else (2/3 = multiple fatal errors, 126/127 =
#          not executable/not found, 128+N = killed by signal N)
#
# Infrastructure guards (each exits 1 on its own; they exist because a
# broken/absent/mis-configured compiler used to produce a FALSE GREEN):
#   1. Positive controls: an empty TU and `#include <cstddef>` must
#      compile (exit 0) in all 8 cells; a negative control, a TU with
#      `#error`, must FAIL (exit 1) in all 4.  Any other outcome = broken
#      harness (a compiler that accepts everything is as useless as one
#      that rejects everything).
#   2. Baseline: if more than half the headers FAIL under gnu++17 in an
#      ABI, the toolchain is not working, so the result is not a valid
#      C++20 measurement.
#   3. Any INFRA cell in the sweep: g++ died abnormally.
#
# C++20 regression (the actual thing being probed, also exit 1):
#   A header PASSES gnu++17 but FAILS (exit 1) gnu++20 in the same ABI.
#
# Exit:
#   0   no regressions and no infrastructure failures;
#       OR the toolchain is entirely absent (a SKIP line, exit 0).
#   1   >=1 C++20 regression, or >=1 infrastructure failure.
#
# Usage:
#   cxx20-header-test.sh [--table FILE] [--self-test]
#     --table FILE   copy the full (untruncated) table to FILE as well.
#     --self-test    run with stub compilers that always exit 1 and always
#                    exit 0, and assert the script exits 1 (infrastructure)
#                    for both, not 0.
#
# POSIX sh.  No Python.

set -u

# ============================================================== Globals
GXX=""
HDR=""
TABLE_FILE=""
SELF_TEST=0

# compile_cell sets these:
#   CC_RESULT  0 = PASS, 1 = FAIL (exit 1), 2 = INFRA (anything else)
#   CC_ERR     first error line (or the stderr head) when not PASS.
CC_RESULT=0
CC_ERR=""

# ============================================================== Arg parse
while [ $# -gt 0 ]; do
    case "$1" in
        --table)
            [ $# -lt 2 ] && { echo "error: --table needs FILE" >&2; exit 2; }
            TABLE_FILE="$2"; shift 2 ;;
        --self-test)
            SELF_TEST=1; shift ;;
        -h|--help)
            echo "usage: cxx20-header-test.sh [--table FILE] [--self-test]"
            exit 0 ;;
        *)
            echo "error: unknown option: $1" >&2; exit 2 ;;
    esac
done

# ============================================================== Locate
# In self-test mode the fake toolchain is provided by run_self_test.
if [ "${SELF_TEST}" != "1" ]; then
    PS3DEV="${PS3DEV:-}"
    if [ -z "$PS3DEV" ] && [ -x "$HOME/ps3dev/ppu/bin/powerpc64-ps3-elf-g++" ]; then
        PS3DEV="$HOME/ps3dev"
    fi

    # Walk up from this script's dir to a root containing sdk/include.
    ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P 2>/dev/null)" || ROOT=""
    while [ -n "$ROOT" ] && [ "$ROOT" != "/" ] && [ ! -d "$ROOT/sdk/include" ]; do
        _rn="$(CDPATH= cd -- "$ROOT/.." 2>/dev/null && pwd -P)" || _rn=""
        [ -z "$_rn" ] && { ROOT=""; break; }
        ROOT="$_rn"
    done
    HDR_SRC="${ROOT:+$ROOT/sdk/include}"
    [ -n "${ROOT:-}" ] && [ ! -d "$HDR_SRC" ] && HDR_SRC=""

    if [ -n "$PS3DEV" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++" ] && [ -d "$PS3DEV/ps3dk/ppu/include" ]; then
        GXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
        HDR="$PS3DEV/ps3dk/ppu/include"
    elif [ -n "${PS3DK:-}" ] && [ -x "$PS3DK/ppu/bin/powerpc64-ps3-elf-g++" ] && [ -d "$PS3DK/ps3dk/ppu/include" ]; then
        GXX="$PS3DK/ppu/bin/powerpc64-ps3-elf-g++"
        HDR="$PS3DK/ps3dk/ppu/include"
    elif command -v powerpc64-ps3-elf-g++ >/dev/null 2>&1 && [ -n "$HDR_SRC" ]; then
        GXX="powerpc64-ps3-elf-g++"
        HDR="$HDR_SRC"
    fi
fi

# ============================================================== compile_cell
# compile_cell <source-line> <std-flag> <mlp64-flag-or-empty>
# Sets CC_RESULT (0/1/2) and CC_ERR.  The g++ command is
#   g++ -fsyntax-only -isystem $HDR [mlp64] <std> -x c++ -
# with source-line on stdin.  Uses ${var:+"$var"} (set-AND-nonempty) so
# the empty ILP32 cell passes NO extra argument (an empty filename would
# make g++ fail).
compile_cell() {
    CC_RESULT=0; CC_ERR=""
    _src="$1"; _std="$2"; _mlp="${3:-}"
    _ef="$(mktemp -t cxx20-cc.XXXXXX 2>/dev/null)" || _ef=""
    _mlparg=""
    [ -n "$_mlp" ] && _mlparg="$_mlp"

    if [ -n "$_ef" ]; then
        printf '%s\n' "$_src" \
            | "$GXX" -fsyntax-only -isystem "$HDR" \
                ${_mlparg:+"$_mlparg"} "$_std" -x c++ - \
            >/dev/null 2>"$_ef"
    else
        printf '%s\n' "$_src" \
            | "$GXX" -fsyntax-only -isystem "$HDR" \
                ${_mlparg:+"$_mlparg"} "$_std" -x c++ - \
            >/dev/null 2>&1
    fi
    _rc=$?

    if [ "$_rc" -eq 0 ]; then
        CC_RESULT=0
    elif [ "$_rc" -eq 1 ]; then
        CC_RESULT=1
    else
        CC_RESULT=2
    fi

    if [ "$CC_RESULT" -ne 0 ]; then
        if [ -n "${_ef:-}" ] && [ -s "$_ef" ]; then
            _raw="$(head -3 "$_ef" 2>/dev/null)"
            _fe="$(printf '%s\n' "$_raw" | grep -E -m1 'error:|Error')"
            if [ -n "$_fe" ]; then CC_ERR="$_fe"; else CC_ERR="$_raw"; fi
        else
            _raw="$(printf '%s\n' "$_src" \
                | "$GXX" -fsyntax-only -isystem "$HDR" \
                    ${_mlparg:+"$_mlparg"} "$_std" -x c++ - 2>&1)"
            _fe="$(printf '%s\n' "$_raw" | grep -E -m1 'error:|Error')"
            if [ -n "$_fe" ]; then CC_ERR="$_fe"; else CC_ERR="$_raw"; fi
        fi
    fi
    rm -f "${_ef:-}" 2>/dev/null
    return 0
}

# ============================================================== run_probe
run_probe() {
    echo "cxx20-header-test"
    echo "  g++   : $GXX"
    echo "  hdr   : $HDR"
    echo ""

    # ---------------- 1. Positive controls (must all PASS) --------------
    echo "  positive controls (all 8 cells must PASS):"
    CTRL_BROKEN=0
    for _csrc in "/* empty TU */" "#include <cstddef>"; do
        for _cstd in "-std=gnu++17" "-std=gnu++20"; do
            for _cmlp in "" "-mlp64"; do
                compile_cell "$_csrc" "$_cstd" "$_cmlp"
                if [ "$CC_RESULT" -eq 0 ]; then
                    _lbl="PASS"; _tail=""
                else
                    CTRL_BROKEN=1
                    if [ "$CC_RESULT" -eq 1 ]; then _lbl="FAIL"; else _lbl="INFRA"; fi
                    _tail=" | ${CC_ERR:-exit code != 0/1}"
                fi
                _clbl="ilp32"; [ -n "$_cmlp" ] && _clbl="lp64"
                _cd="$(printf '%s' "$_csrc" | cut -c1-26)"
                printf '    %-5s  %-13s  %-5s  %s%s\n' \
                    "$_lbl" "$_cstd" "$_clbl" "$_cd" "$_tail"
            done
        done
    done
    echo "  negative control (all 4 cells must FAIL with exit 1):"
    for _cstd in "-std=gnu++17" "-std=gnu++20"; do
        for _cmlp in "" "-mlp64"; do
            compile_cell "#error negative control" "$_cstd" "$_cmlp"
            if [ "$CC_RESULT" -eq 1 ]; then _lbl="FAIL"; else CTRL_BROKEN=1; _lbl="BAD"; fi
            _clbl="ilp32"; [ -n "$_cmlp" ] && _clbl="lp64"
            printf '    %-5s  %-13s  %-5s  #error\n' "$_lbl" "$_cstd" "$_clbl"
        done
    done
    if [ "${CTRL_BROKEN}" -ne 0 ]; then
        echo ""
        echo "  INFRASTRUCTURE FAILURE: a control did not behave (positive must compile, #error must fail)."
        echo "  The compiler environment is not usable; this is not a valid C++20 result."
        return 1
    fi
    echo ""

    # ---------------- 2. Enumerate --------------------------------------
    _list="$(mktemp -t cxx20.List.XXXXXX)"
    find "$HDR" -type f -name '*.h' 2>/dev/null \
        | sed -e "s|^$HDR/||" | sort > "$_list"
    N_HDR="$(wc -l < "$_list" 2>/dev/null)"; N_HDR="${N_HDR:-0}"
    if [ "${N_HDR}" -eq 0 ]; then
        rm -f "$_list"
        echo "  SKIP: no .h files under $HDR"
        return 0
    fi
    echo "  headers to probe: $N_HDR"
    echo ""

    # ---------------- 3. Sweep ------------------------------------------
    REGR=0
    SWEEP_INFRA=0
    PASS17_IL=0
    PASS17_LP=0
    TABLE="$(mktemp -t cxx20.Table.XXXXXX)"
    REGRS="$(mktemp -t cxx20.Regrs.XXXXXX)"
    : > "$REGRS"

    printf '  %-5s  %-5s  %-5s  %-5s  %s\n' \
        "17-il" "17-lp" "20-il" "20-lp" "header" > "$TABLE"

    while IFS= read -r rel; do
        [ -z "$rel" ] && continue
        _inc="#include <$rel>"
        p17i=0; p17l=0; p20i=0; p20l=0
        E20I=""; E20L=""

        compile_cell "$_inc" "-std=gnu++17" ""
        [ "$CC_RESULT" -eq 0 ] && p17i=1
        [ "$CC_RESULT" -eq 2 ] && { SWEEP_INFRA=1; break; }

        compile_cell "$_inc" "-std=gnu++17" "-mlp64"
        [ "$CC_RESULT" -eq 0 ] && p17l=1
        [ "$CC_RESULT" -eq 2 ] && { SWEEP_INFRA=1; break; }

        compile_cell "$_inc" "-std=gnu++20" ""
        [ "$CC_RESULT" -eq 0 ] && p20i=1; E20I="$CC_ERR"
        [ "$CC_RESULT" -eq 2 ] && { SWEEP_INFRA=1; break; }

        compile_cell "$_inc" "-std=gnu++20" "-mlp64"
        [ "$CC_RESULT" -eq 0 ] && p20l=1; E20L="$CC_ERR"
        [ "$CC_RESULT" -eq 2 ] && { SWEEP_INFRA=1; break; }

        [ "$p17i" -eq 1 ] && PASS17_IL=$((PASS17_IL + 1))
        [ "$p17l" -eq 1 ] && PASS17_LP=$((PASS17_LP + 1))

        A17I="FAIL"; A17L="FAIL"; A20I="FAIL"; A20L="FAIL"
        [ "$p17i" -eq 1 ] && A17I="PASS"
        [ "$p17l" -eq 1 ] && A17L="PASS"
        [ "$p20i" -eq 1 ] && A20I="PASS"
        [ "$p20l" -eq 1 ] && A20L="PASS"
        printf '  %-5s  %-5s  %-5s  %-5s  %s\n' \
            "$A17I" "$A17L" "$A20I" "$A20L" "$rel" >> "$TABLE"

        if [ "$p17i" -eq 1 ] && [ "$p20i" -eq 0 ]; then
            REGR=$((REGR + 1))
            printf '  REGRESSION [ilp32] %s\n    %s\n' \
                "$rel" "${E20I:-<no error line captured>}" >> "$REGRS"
        fi
        if [ "$p17l" -eq 1 ] && [ "$p20l" -eq 0 ]; then
            REGR=$((REGR + 1))
            printf '  REGRESSION [lp64 ] %s\n    %s\n' \
                "$rel" "${E20L:-<no error line captured>}" >> "$REGRS"
        fi
    done < "$_list"
    rm -f "$_list"

    # ---------------- 4. Baseline sanity --------------------------------
    _infra_msg=""
    if [ "${SWEEP_INFRA}" -ne 0 ]; then
        _infra_msg="sweep aborted: a compile cell exited abnormally (not 0 or 1)"
    fi
    if [ -z "$_infra_msg" ] && [ "${N_HDR}" -gt 0 ]; then
        if [ $(( 2 * PASS17_IL )) -lt "$N_HDR" ]; then
            _infra_msg="baseline: only $PASS17_IL/$N_HDR headers pass gnu++17 ILP32 (majority fail)"
        elif [ $(( 2 * PASS17_LP )) -lt "$N_HDR" ]; then
            _infra_msg="baseline: only $PASS17_LP/$N_HDR headers pass gnu++17 LP64 (majority fail)"
        fi
    fi

    # ---------------- 5. Report -----------------------------------------
    echo "  full table ($N_HDR headers):"
    cat "$TABLE"
    if [ -n "$TABLE_FILE" ]; then
        cp "$TABLE" "$TABLE_FILE" 2>/dev/null \
            && echo "  table written to: $TABLE_FILE"
    fi
    echo ""

    if [ -n "$_infra_msg" ]; then
        echo "  INFRASTRUCTURE FAILURE: $_infra_msg"
        echo "  Not a valid C++20 result."
        rm -f "$TABLE" "$REGRS"
        return 1
    fi

    if [ "$REGR" -gt 0 ]; then
        echo "  C++20 regressions (passed gnu++17, failed gnu++20):"
        cat "$REGRS"
        echo ""
        echo "CXX20_HEADER_TEST_FAIL"
        rm -f "$TABLE" "$REGRS"
        return 1
    fi

    echo "CXX20_HEADER_TEST_OK"
    rm -f "$TABLE" "$REGRS"
    return 0
}

# ============================================================== self-test
# A stub g++ that always exits 1 must drive the probe to exit 1
# (infrastructure: positive controls fail), NOT a false-green exit 0.
run_self_test() {
    _st_status=0
    for _stub_rc in 1 0; do
        _stk="$(mktemp -d)"
        mkdir -p "$_stk/ppu/bin" "$_stk/ps3dk/ppu/include"
        _gxx="$_stk/ppu/bin/powerpc64-ps3-elf-g++"
        printf '#!/bin/sh\nexit %s\n' "$_stub_rc" > "$_gxx"
        chmod +x "$_gxx" 2>/dev/null
        printf 'extern int fake_a;\n' > "$_stk/ps3dk/ppu/include/fake_a.h"
        printf 'extern int fake_b;\n' > "$_stk/ps3dk/ppu/include/fake_b.h"

        GXX="$_gxx"
        HDR="$_stk/ps3dk/ppu/include"

        echo "self-test: running probe against an always-exit-$_stub_rc stub compiler"
        run_probe
        _rc=$?
        rm -rf "$_stk"

        if [ "$_rc" -eq 1 ]; then
            echo "self-test: PASS (always-exit-$_stub_rc stub detected as infrastructure, exit=1)"
        else
            echo "self-test: FAIL - always-exit-$_stub_rc stub produced exit $_rc (a false green would be 0); expected 1" >&2
            _st_status=2
        fi
    done
    return $_st_status
}

# ============================================================== main
if [ "${SELF_TEST}" = "1" ]; then
    run_self_test
    exit $?
fi

if [ -z "$GXX" ]; then
    echo "SKIP cxx20-header-test: no PPU g++ reachable (need \$PS3DEV or powerpc64-ps3-elf-g++ on PATH, and ps3dk/ppu/include)"
    exit 0
fi

run_probe
exit $?

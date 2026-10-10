#!/usr/bin/env bash
# tool-alias's error message must stay inside its buffer.
#
# fail() formats "tool-alias: <what>: <detail>\r\n" into a 584-character
# buffer with _snwprintf.  When the text does not fit, _snwprintf returns -1
# and writes no terminator, and the old code then took wcslen() of the
# unterminated buffer: it read past the end and wrote whatever followed.
# A missing target with a long (long-path) directory reaches that path.
#
# Rows, through tests/host/tool-alias-fail-test.c with stderr in a file:
#   short  the whole message, exactly as formatted;
#   long   an 800-character detail: exactly 583 characters, starting with
#          "tool-alias: missing: ZZZ" and ending in the CR LF the cut adds,
#          with nothing after it.
# The harness must exit 0 in every row.
#
# SKIP only when a capability is shown to be absent: no Windows C compiler
# (x86_64-w64-mingw32-gcc, or MSVC cl through vcvars64.bat from a Windows
# shell), or no way to run Windows programs (neither a Windows shell nor WSL
# interop).  A compiler that is found but fails, or a harness that exits
# non-zero where Windows programs run, is a FAIL.
#
# Controls (when the rows pass): the script re-runs itself with
# TOOL_ALIAS_FAIL_TEST_CC set to a stub "compiler" that fails, and to one
# that produces a program exiting 3; both must make it exit 1.
#
# Usage: tests/host/tool-alias-fail-test.sh
set -u
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
exe="$work/fail-test.exe"
src="$here/tool-alias-fail-test.c"

can_run_windows() {
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; esac
    [ -e /proc/sys/fs/binfmt_misc/WSLInterop ]
}

# compile <out.exe> <source.c>
kind=""
if [ -n "${TOOL_ALIAS_FAIL_TEST_CC:-}" ]; then
    kind=override
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    kind=mingw
elif command -v cmd.exe >/dev/null 2>&1 && command -v cygpath >/dev/null 2>&1; then
    vcvars="$(ls /c/Program\ Files/Microsoft\ Visual\ Studio/*/*/VC/Auxiliary/Build/vcvars64.bat 2>/dev/null | head -n 1)"
    [ -n "$vcvars" ] && kind=msvc
fi
compile() {
    case "$kind" in
    override) "$TOOL_ALIAS_FAIL_TEST_CC" "$1" "$2" ;;
    mingw) x86_64-w64-mingw32-gcc -O2 -Wall -municode -static -o "$1" "$2" ;;
    msvc)
        # A batch file sidesteps cmd.exe's quoting of a path with spaces.
        local out_dir out_name
        out_dir="$(dirname "$1")"; out_name="$(basename "$1")"
        {
            printf '@call "%s" >nul 2>&1\r\n' "$(cygpath -w "$vcvars")"
            printf '@cd /d "%s"\r\n' "$(cygpath -w "$out_dir")"
            printf '@cl /nologo /O2 /W3 /Fe%s "%s" >nul\r\n' "$out_name" "$(cygpath -w "$2")"
            printf '@exit /b %%errorlevel%%\r\n'
        } > "$out_dir/build-$out_name.bat"
        MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$out_dir/build-$out_name.bat")" && [ -f "$1" ]
        ;;
    esac
}

if [ -z "$kind" ]; then
    echo "tool-alias-fail: SKIP (no Windows C compiler)"
    exit 0
fi
if ! can_run_windows; then
    echo "tool-alias-fail: SKIP (Windows programs cannot run on this host)"
    exit 0
fi
if ! compile "$exe" "$src"; then
    echo "tool-alias-fail: FAIL ($kind) the harness did not compile"
    exit 1
fi

fail=0
if "$exe" short 2> "$work/short.txt"; then
    printf 'tool-alias: missing: C:/sdk/ppu/bin/powerpc64-ps3-elf-gcc.exe\r\n' > "$work/short.want"
    if cmp -s "$work/short.txt" "$work/short.want"; then
        echo "tool-alias-fail: ok   short message intact"
    else
        echo "tool-alias-fail: FAIL short message: $(od -c "$work/short.txt" | head -n 3)"; fail=1
    fi
else
    echo "tool-alias-fail: FAIL short row: harness exited $?"; fail=1
fi

if "$exe" long 2> "$work/long.txt"; then
    n=$(wc -c < "$work/long.txt")
    head_ok=$(head -c 24 "$work/long.txt")
    tail_hex=$(tail -c 2 "$work/long.txt" | od -An -tx1 | tr -d ' \n')
    body_z=$(head -c 581 "$work/long.txt" | tail -c +22 | tr -d 'Z' | wc -c)
    if [ "$n" -eq 583 ] && [ "$head_ok" = "tool-alias: missing: ZZZ" ] && [ "$tail_hex" = 0d0a ] && [ "$body_z" -eq 0 ]; then
        echo "tool-alias-fail: ok   long message cut at 583 characters, CR LF, nothing past the buffer"
    else
        echo "tool-alias-fail: FAIL long message: $n bytes, head '$head_ok', tail $tail_hex, non-Z in body $body_z"; fail=1
    fi
else
    echo "tool-alias-fail: FAIL long row: harness exited $?"; fail=1
fi

# Controls, from the top-level run only, against the same compiler.
if [ "$fail" -eq 0 ] && [ "$kind" != override ]; then
    printf '#!/usr/bin/env bash\nexit 1\n' > "$work/cc-fails"
    printf 'int wmain (void) { return 3; }\n' > "$work/exit3.c"
    if compile "$work/exit3.exe" "$work/exit3.c"; then
        printf '#!/usr/bin/env bash\ncp "%s" "$1"\n' "$work/exit3.exe" > "$work/cc-exit3"
    else
        echo "tool-alias-fail: FAIL ($kind) the exit-3 control did not compile"; fail=1
    fi
    chmod +x "$work/cc-fails" "$work/cc-exit3" 2>/dev/null
    for stub in cc-fails cc-exit3; do
        [ -f "$work/$stub" ] || continue
        TOOL_ALIAS_FAIL_TEST_CC="$work/$stub" bash "$0" > "$work/$stub.out" 2>&1; rc=$?
        if [ "$rc" -eq 1 ] && grep -q FAIL "$work/$stub.out"; then
            echo "tool-alias-fail: ok   control $stub fails the test"
        else
            echo "tool-alias-fail: FAIL control $stub: exit $rc"; sed 's/^/    /' "$work/$stub.out"; fail=1
        fi
    done
fi

[ "$fail" -eq 0 ] && echo "tool-alias-fail: PASS ($kind)"
exit "$fail"

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
#
# Needs a Windows C compiler and a way to run its output: x86_64-w64-mingw32
# gcc where Windows programs can run (Windows, or WSL with interop), or MSVC
# cl on Windows.  Skips otherwise.
#
# Usage: tests/host/tool-alias-fail-test.sh
set -u
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
exe="$work/fail-test.exe"

built=""
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    x86_64-w64-mingw32-gcc -O2 -Wall -municode -static -o "$exe" "$here/tool-alias-fail-test.c" && built=mingw
fi
if [ -z "$built" ] && command -v cmd.exe >/dev/null 2>&1; then
    vcvars="$(ls /c/Program\ Files/Microsoft\ Visual\ Studio/*/*/VC/Auxiliary/Build/vcvars64.bat 2>/dev/null | head -n 1)"
    if [ -n "$vcvars" ]; then
        # A batch file sidesteps cmd.exe's quoting of a path with spaces.
        {
            printf '@call "%s" >nul 2>&1\r\n' "$(cygpath -w "$vcvars")"
            printf '@cd /d "%s"\r\n' "$(cygpath -w "$work")"
            printf '@cl /nologo /O2 /W3 /Fefail-test.exe "%s" >nul\r\n' "$(cygpath -w "$here/tool-alias-fail-test.c")"
        } > "$work/build.bat"
        MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$work/build.bat")" && [ -f "$exe" ] && built=msvc
    fi
fi
if [ -z "$built" ]; then
    echo "tool-alias-fail: SKIP (no Windows C compiler)"
    exit 0
fi
if ! "$exe" short 2>/dev/null; then
    echo "tool-alias-fail: SKIP (built with $built, but Windows programs do not run here)"
    exit 0
fi

fail=0
"$exe" short 2> "$work/short.txt"
printf 'tool-alias: missing: C:/sdk/ppu/bin/powerpc64-ps3-elf-gcc.exe\r\n' > "$work/short.want"
if cmp -s "$work/short.txt" "$work/short.want"; then
    echo "tool-alias-fail: ok   short message intact"
else
    echo "tool-alias-fail: FAIL short message: $(od -c "$work/short.txt" | head -n 3)"; fail=1
fi

"$exe" long 2> "$work/long.txt"
n=$(wc -c < "$work/long.txt")
head_ok=$(head -c 24 "$work/long.txt")
tail_hex=$(tail -c 2 "$work/long.txt" | od -An -tx1 | tr -d ' \n')
body_z=$(head -c 581 "$work/long.txt" | tail -c +22 | tr -d 'Z' | wc -c)
if [ "$n" -eq 583 ] && [ "$head_ok" = "tool-alias: missing: ZZZ" ] && [ "$tail_hex" = 0d0a ] && [ "$body_z" -eq 0 ]; then
    echo "tool-alias-fail: ok   long message cut at 583 characters, CR LF, nothing past the buffer"
else
    echo "tool-alias-fail: FAIL long message: $n bytes, head '$head_ok', tail $tail_hex, non-Z in body $body_z"; fail=1
fi

[ "$fail" -eq 0 ] && echo "tool-alias-fail: PASS ($built)"
exit "$fail"

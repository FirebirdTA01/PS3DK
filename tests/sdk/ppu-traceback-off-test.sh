#!/usr/bin/env bash
# Codegen guard for GCC patch 0085: on the Cell PPU the AIX traceback
# table (tbtab) is dropped by default, but an explicit -mtraceback
# must still override.
#
# The tbtab is emitted by rs6000-logue.cc at the end of each function
# in .text as:
#     <fn>LT<fn>:
#     .long 0
#     .byte 0,<lang>,...
# after `blr`, before the next function/section.
#
# The gate is:
#     (DEFAULT_ABI == ABI_AIX || DEFAULT_ABI == ABI_ELFv2)
#      && rs6000_traceback != traceback_none
#      && ! cfun->is_thunk
#
# So for a single non-thunk function:
#     default                 -> 0 tbtabs
#     -mtraceback=full        -> 1 tbtab
#     -mtraceback=part        -> 1 tbtab
#     -mtraceback=no          -> 0 tbtabs
#
# Skips without PS3DEV.
# Usage: tests/sdk/ppu-traceback-off-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-traceback-off: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-traceback-off: FAIL: no PPU C compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.c" <<'EOF'
int f (int a, int b) { return a + b; }
EOF

fail=0

# Count tbtab occurrences: each function's tbtab starts with a
# ".long 0" followed by ".byte 0,".  In a clean -S dump of a single
# function the tbtab (if present) shows up as:
#   .long 0
#   .byte 0,
# Count the ".byte 0," marker (the first byte of the tbtab body).
count_tbtab() {  # $1 = .s file
  # A tbtab starts with a .long 0 all-zero word immediately followed by
  # the .byte 0, (format-type) header line.  Match that two-line pair.
  awk '
    /^[[:space:]]*\.long[[:space:]]+0[[:space:]]*$/ { pending = 1; next }
    pending && /^[[:space:]]*\.byte[[:space:]]+0,/{ n++; pending = 0 }
    { pending = 0 }
    END { print n + 0 }
  ' "$1"
}

# --- default: 0 tbtab -----------------------------------------------------
if "$cc" -S "$work/t.c" -o "$work/t.s" 2>"$work/err"; then
  n=$(count_tbtab "$work/t.s")
  if [ "$n" -eq 0 ]; then
    echo "ppu-traceback-off: ok   default: no tbtab in asm"
  else
    echo "ppu-traceback-off: FAIL default: $n tbtab(s) present (0085 not applied?)"
    fail=1
  fi
else
  echo "ppu-traceback-off: FAIL default compile: $(cat "$work/err" | head -3)"
  fail=1
fi

# --- -mtraceback=full: 1 tbtab (user override preserved) ------------------
if "$cc" -mtraceback=full -S "$work/t.c" -o "$work/t.s" 2>"$work/err"; then
  n=$(count_tbtab "$work/t.s")
  if [ "$n" -ge 1 ]; then
    echo "ppu-traceback-off: ok   -mtraceback=full: $n tbtab(s) present (override preserved)"
  else
    echo "ppu-traceback-off: FAIL -mtraceback=full: 0 tbtab(s) (override clobbered)"
    fail=1
  fi
else
  echo "ppu-traceback-off: FAIL -mtraceback=full compile: $(cat "$work/err" | head -3)"
  fail=1
fi

# --- -mtraceback=no: 0 tbtab ----------------------------------------------
if "$cc" -mtraceback=no -S "$work/t.c" -o "$work/t.s" 2>"$work/err"; then
  n=$(count_tbtab "$work/t.s")
  if [ "$n" -eq 0 ]; then
    echo "ppu-traceback-off: ok   -mtraceback=no: no tbtab in asm"
  else
    echo "ppu-traceback-off: FAIL -mtraceback=no: $n tbtab(s) present"
    fail=1
  fi
else
  echo "ppu-traceback-off: FAIL -mtraceback=no compile: $(cat "$work/err" | head -3)"
  fail=1
fi

[ $fail -eq 0 ] && echo "ppu-traceback-off: PASS"
exit $fail

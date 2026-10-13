#!/usr/bin/env bash
# tests/sdk/np-path-forwarders-test.sh
#
# Self-test for the np/ tree forwarders. Run it after an SDK has been
# installed and its compilers are on PATH (powerpc64-ps3-elf-gcc/g++); the
# cross compiler ships with its own sysroot, so no --sysroot is passed.
#
# It compiles (it does not link) a set of small TUs and asserts that:
#   * the reference TU compiles under both C (gnu99) and C++ (c++17);
#   * each of the 11 forwarders compiles alone in its own TU;
#   * a "ranking" control compiles (it uses a symbol that only lives in
#     cell/np/score.h, so it distinguishes the correct target from a wrong
#     one);
#   * a self-check where <np/ranking.h> is pointed at the WRONG cell header
#     (cell/np/basic.h) makes the same control FAIL while a basic-header
#     control still PASSES (proving the check is sensitive to the target).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
inc="$repo/sdk/include"
ref="$here/np-path-forwarders-test.c"
[ -f "$ref" ] || { echo "ref TU $ref not found" >&2; exit 1; }

CC="${CC:-powerpc64-ps3-elf-gcc}"
CXX="${CXX:-powerpc64-ps3-elf-g++}"
for tool in "$CC" "$CXX"; do
  command -v "$tool" >/dev/null 2>&1 || { echo "need $tool on PATH" >&2; exit 2; }
done

ov="$(mktemp -d)"
ovb="$(mktemp -d)"
trap 'rm -rf "$ov" "$ovb"' EXIT

lone=(basic custom_menu friendlist lookup manager profile ranking signaling sns util auth_oauth)

# Overlay: ONLY the 11 new forwarders, placed in front of the SDK include dir.
ovinc="$ov/np"
mkdir -p "$ovinc"
for f in "${lone[@]}"; do cp "$inc/np/$f.h" "$ovinc/$f.h"; done

fail=0
run_ok()  { local label="$1"; shift; if "$@" >/dev/null 2>"$ov/e"; then :; else echo "FAIL(ok)  $label"; sed -n '1,14p' "$ov/e"; fail=1; fi; }
run_bad() { local label="$1"; shift; if "$@" >/dev/null 2>&1; then echo "FAIL(bad) $label compiled when it should not"; fail=1; fi; }

# 1) matrix: the reference TU (all 11 <np/X.h> included) under C and C++.
run_ok "C   reference" "$CC"  -x c   -std=gnu99 -Wall -Wextra -Werror -I"$ov" -I"$inc" -c "$ref" -o "$ov/ref.o"
run_ok "C++ reference" "$CXX" -x c++ -std=c++17 -Wall -Wextra -Werror -I"$ov" -I"$inc" -c "$ref" -o "$ov/ref.o"

# 2) each forwarder alone.
for f in "${lone[@]}"; do
  printf '#include <np/%s.h>\n' "$f" > "$ov/lone_$f.c"
  run_ok "C lone $f" "$CC" -std=gnu99 -Wall -Wextra -Werror -I"$ov" -I"$inc" -c "$ov/lone_$f.c" -o "$ov/lone_$f.o"
done

# 3) control: <np/ranking.h> must resolve to cell/np/score.h (which declares
#    sceNpScoreAbortTransaction); a bare use of the symbol type-checks.
printf '%s\n' '#include <np/ranking.h>' 'int control_ranking(void)' '{' '  return &sceNpScoreAbortTransaction ? 1 : 0;' '}' > "$ov/control.c"
run_ok "C control ranking" "$CC" -std=gnu99 -Wall -Wextra -Werror -I"$ov" -I"$inc" -c "$ov/control.c" -o "$ov/control.o"

# 4) self-check: with <np/ranking.h> pointed at the WRONG target
#    (cell/np/basic.h), the ranking control must FAIL and a basic control
#    must still PASS.
mkdir -p "$ovb/np"
printf '%s\n' '#include <cell/np/basic.h>' > "$ovb/np/ranking.h"
printf '%s\n' '#include <cell/np/basic.h>' > "$ovb/np/basic.h"
run_bad  "self-check bad ranking rejected" "$CC" -std=gnu99 -Wall -Wextra -Werror -I"$ovb" -I"$inc" -c "$ov/control.c" -o "$ovb/control.o"
printf '%s\n' '#include <np/basic.h>' 'int control_basic(void)' '{' '  return &sceNpBasicInit ? 1 : 0;' '}' > "$ov/control_basic.c"
run_ok   "self-check basic still OK" "$CC" -std=gnu99 -Wall -Wextra -Werror -I"$ovb" -I"$inc" -c "$ov/control_basic.c" -o "$ovb/control_basic.o"

if [ "$fail" -ne 0 ]; then echo "self-test FAIL" >&2; exit 1; fi
echo "self-test OK: 2 matrix + ${#lone[@]} lone + 1 control + 2 self-check"

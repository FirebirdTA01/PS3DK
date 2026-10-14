#!/usr/bin/env bash
# Codegen guard for GCC patch 0029.  Under the ILP32 data model a pointer is
# 32 bits and the ABI zero-extends it (POINTERS_EXTEND_UNSIGNED).  A DIRECT
# conversion of a pointer to a 64-bit integer, signed or unsigned, must
# therefore zero-extend; without 0029, convert.cc used a signed intermediate
# and the conversion sign-extended (extsw), so an address at or above
# 0x80000000 became negative.
#
# Each conversion is its own extern "C" function, and only that function's
# body is scanned, so legitimate sign extensions elsewhere cannot fail it:
#   wid_u64      (unsigned long long) p          must not sign-extend
#   wid_s64      (long long) p                   must not sign-extend
#   wid_uintptr  (uint64_t)(uintptr_t) p         must not sign-extend (unsigned control)
#   wid_intptr   (int64_t)(intptr_t) p           MUST sign-extend (signed contrast:
#                                                intptr_t is a 32-bit int in ILP32)
# "Sign-extend" means an extsw or an lwa in the body.  C and C++, ILP32,
# -O0/-O1/-O2/-O3/-Os.  Every row removes its output first and requires the
# compile to exit 0 with a non-empty listing; a probe function missing from
# the listing fails its row.  The runtime half is tests/regression/ilp32-ptr-widen.
# Controls (embedded): a wid_u64 body with extsw and a wid_intptr body
# without one must be rejected; a correct listing must pass.
#
# Skips the compile leg without PS3DEV (CI has no PPU compiler); the
# controls always run.
# usage: ilp32-pointer-widen-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# scan <listing>: one line per probe function; exit 1 on any violation or missing body.
cat > "$work/scan.sh" <<'EOF'
#!/usr/bin/env bash
awk '
  function done_fn() {
      if (fn == "") return
      seen[fn] = 1
      if (fn == "wid_intptr") { if (!sx) { print "  " fn ": no sign extension"; bad++ } }
      else if (sx) { print "  " fn ": sign-extends (" sxline ")"; bad++ }
      fn = ""
  }
  {
      line = $0; sub(/^[ \t]+/, "", line)
      if (match(line, /^\.L\.wid_(u64|s64|uintptr|intptr):$/)) {
          done_fn(); fn = substr(line, 4, length(line) - 4); sx = 0; next
      }
      if (fn == "") next
      if (line ~ /^\.cfi_endproc/) { done_fn(); next }
      if (line ~ /^(extsw|extsw\.|lwa|lwax)[ \t]/) { sx = 1; sxline = line }
  }
  END {
      done_fn()
      n = split("wid_u64 wid_s64 wid_uintptr wid_intptr", want, " ")
      for (i = 1; i <= n; i++) if (!(want[i] in seen)) { print "  " want[i] ": not found"; bad++ }
      exit bad ? 1 : 0
  }' "$1"
EOF
chmod +x "$work/scan.sh"
status=0
mk() {  # mk <file> <u64 body> <intptr body>
    {
        for f in wid_u64 wid_s64 wid_uintptr wid_intptr; do
            echo ".L.$f:"
            echo "	.cfi_startproc"
            case $f in wid_u64) echo "	$2" ;; wid_intptr) echo "	$3" ;; *) echo "	clrldi 3,3,32" ;; esac
            echo "	blr"
            echo "	.cfi_endproc"
        done
    } > "$1"
}
mk "$work/good.s" "clrldi 3,3,32" "extsw 3,3"
mk "$work/red1.s" "extsw 3,3" "extsw 3,3"
mk "$work/red2.s" "clrldi 3,3,32" "clrldi 3,3,32"
"$work/scan.sh" "$work/good.s" > /dev/null; g=$?
"$work/scan.sh" "$work/red1.s" > /dev/null; r1=$?
"$work/scan.sh" "$work/red2.s" > /dev/null; r2=$?
head -n 3 "$work/good.s" > "$work/red3.s"
"$work/scan.sh" "$work/red3.s" > /dev/null; r3=$?
if [ $g -eq 0 ] && [ $r1 -eq 1 ] && [ $r2 -eq 1 ] && [ $r3 -eq 1 ]; then
    echo "ilp32-pointer-widen: ok   controls: correct listing accepted; sign-extending direct widening, missing intptr sign extension and missing bodies rejected"
else
    echo "ilp32-pointer-widen: FAIL controls (good $g, red1 $r1, red2 $r2, red3 $r3)"; status=1
fi

if [ -z "$ps3dev" ]; then
    echo "ilp32-pointer-widen: SKIP compile leg (set PS3DEV or --ps3dev)"
    [ $status -eq 0 ] && echo "PASS: ilp32-pointer-widen-codegen-test (controls only)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"; cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ilp32-pointer-widen: FAIL: no PPU compiler under $ps3dev"; exit 1; }
extra=${EXTRA_CFLAGS:-}
cat > "$work/probe.c" <<'EOF'
#include <stdint.h>
unsigned long long wid_u64 (void *p) { return (unsigned long long) p; }
long long wid_s64 (void *p) { return (long long) p; }
uint64_t wid_uintptr (void *p) { return (uint64_t) (uintptr_t) p; }
int64_t wid_intptr (void *p) { return (int64_t) (intptr_t) p; }
EOF
cat > "$work/probe.cc" <<'EOF'
#include <cstdint>
extern "C" {
unsigned long long wid_u64 (void *p) { return (unsigned long long) p; }
long long wid_s64 (void *p) { return reinterpret_cast<long long> (p); }
std::uint64_t wid_uintptr (void *p) { return (std::uint64_t) reinterpret_cast<std::uintptr_t> (p); }
std::int64_t wid_intptr (void *p) { return (std::int64_t) reinterpret_cast<std::intptr_t> (p); }
}
EOF
for lang in c c++; do
    drv=$cc; src="$work/probe.c"; [ "$lang" = c++ ] && { drv=$cxx; src="$work/probe.cc"; }
    for opt in -O0 -O1 -O2 -O3 -Os; do
        label="$lang ilp32 $opt"
        rm -f "$work/p.s" "$work/err"
        # shellcheck disable=SC2086
        "$drv" $extra $opt -S "$src" -o "$work/p.s" 2> "$work/err"; rc=$?
        if [ $rc -ne 0 ] || [ ! -s "$work/p.s" ]; then
            echo "ilp32-pointer-widen: FAIL $label: compile exit $rc"; sed 's/^/    /' "$work/err" | head -3; status=1; continue
        fi
        out=$("$work/scan.sh" "$work/p.s"); rc=$?
        if [ $rc -eq 0 ]; then echo "ilp32-pointer-widen: ok   $label"
        else echo "ilp32-pointer-widen: FAIL $label"; printf '%s\n' "$out"; status=1; fi
    done
done
[ $status -eq 0 ] && echo "PASS: ilp32-pointer-widen-codegen-test"
exit $status

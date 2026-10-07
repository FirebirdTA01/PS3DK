#!/usr/bin/env bash
# SPU GCC must not turn a call into a sibling call when -fcall-saved-N makes
# an argument register the call loads call-saved: the sibcall epilogue
# restores that register over the outgoing argument (and the argument setup
# is then deleted as dead), so the callee receives the caller's old value.
# Those calls must become ordinary brsl calls.  Calls whose argument
# registers all stay call-clobbered must still be sibling calls.
#
# usage: spu-sibcall-call-saved-test.sh [--ps3dev DIR] [--cc-flag FLAG]...
#   --cc-flag adds a compiler flag to every compile (e.g. -B<dir with cc1>
#   to judge a candidate cc1 with the installed driver)
set -u
ps3dev="${PS3DEV:-}"
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --cc-flag) extra+=("$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--cc-flag FLAG]..." >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spu-sibcall-call-saved: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/spu/bin/spu-elf-gcc"
status=0
fail() { echo "spu-sibcall-call-saved: FAIL: $*"; status=1; }
ok() { echo "spu-sibcall-call-saved: ok   $*"; }
[ -x "$cc" ] || [ -x "$cc.exe" ] || { fail "no SPU compiler under $ps3dev"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat > "$work/t.c" <<'SRC'
struct two { int v[8]; };   /* 32 bytes: two argument registers */
extern int g3(int, int, int);
extern int g6(int, int, int, int, int, int);
extern int gs(struct two, int);
/* outgoing $3..$5 */
int f3(int a, int b, int c) { return g3(a + 1, b + 2, c + 3); }
/* outgoing $3..$8, permuted */
int f6(int a, int b, int c, int d, int e, int f) { return g6(f, e, d, c, b, a); }
/* $4/$5 written only for the call */
int f3k(int a) { return g3(a, 7, 9); }
/* $3..$4 hold the struct, $5 the int */
int fs(struct two s, int y) { s.v[0]++; return gs(s, y + 1); }
SRC

# body FILE FN: the assembly of function FN
body() { sed -n "/^$2:/,/\.size[[:space:]]*$2,/p" "$1"; }
compile() { # NAME FLAGS...
    local name="$1"; shift
    if "$cc" -O2 "${extra[@]}" "$@" -S "$work/t.c" -o "$work/$name.s" > "$work/$name.log" 2>&1; then
        return 0
    fi
    fail "$name: compile failed: $(head -1 "$work/$name.log")"
    return 1
}
# refused NAME FN CALLEE: FN calls CALLEE with brsl and has no tail branch
refused() {
    local b; b=$(body "$work/$1.s" "$2")
    [ -n "$b" ] || { fail "$1: no $2 in the output"; return; }
    if grep -Eq '^[[:space:]]*brsl[[:space:]]+\$lr,'"$3"'$' <<<"$b" \
            && ! grep -Eq '^[[:space:]]*br[[:space:]]+'"$3"'$' <<<"$b"; then
        ok "$1: $2 calls $3 with brsl"
    else
        fail "$1: $2 is a sibling call to $3 (an argument register is call-saved)"
    fi
}
# sibcall NAME FN CALLEE: FN still tail-branches to CALLEE
sibcall() {
    local b; b=$(body "$work/$1.s" "$2")
    [ -n "$b" ] || { fail "$1: no $2 in the output"; return; }
    if grep -Eq '^[[:space:]]*br[[:space:]]+'"$3"'$' <<<"$b" \
            && ! grep -Eq "brsl" <<<"$b"; then
        ok "$1: $2 is still a sibling call to $3"
    else
        fail "$1: $2 lost its sibling call to $3"
    fi
}

# Argument registers made call-saved: every affected call must be refused.
if compile cs4 -fcall-saved-4; then
    refused cs4 f3 g3
    # the b + 2 setup into $4 must survive (the bug deleted it)
    if body "$work/cs4.s" f3 | grep -Eq '^[[:space:]]*ai[[:space:]]+\$4,\$[0-9]+,2$'; then
        ok "cs4: f3 still computes b + 2 into \$4"
    else
        fail "cs4: f3 lost the b + 2 argument setup"
    fi
    refused cs4 f6 g6
    refused cs4 f3k g3
    refused cs4 fs gs
fi
# $5 is the LAST argument register f3, f3k and fs load
if compile cs5 -fcall-saved-5; then
    refused cs5 f3 g3
    refused cs5 f3k g3
    refused cs5 fs gs
    refused cs5 f6 g6
fi
# $8 is the last of f6's six
if compile cs8 -fcall-saved-8; then
    refused cs8 f6 g6
    sibcall cs8 f3 g3
    sibcall cs8 f3k g3
    sibcall cs8 fs gs
fi

# Controls: default registers, and call-saved registers outside the
# argument registers, keep every sibling call.
if compile plain; then
    for f in f3:g3 f6:g6 f3k:g3 fs:gs; do sibcall plain "${f%%:*}" "${f#*:}"; done
fi
if compile cs75 -fcall-saved-75 -fcall-saved-76 -fcall-saved-77 -fcall-saved-78 -fcall-saved-79; then
    for f in f3:g3 f6:g6 f3k:g3 fs:gs; do sibcall cs75 "${f%%:*}" "${f#*:}"; done
fi

[ "$status" -eq 0 ] && echo "spu-sibcall-call-saved: PASS"
exit $status

#!/usr/bin/env bash
# Guard for newlib patch 0018: on the PS3 LV2 target the E* errno names are
# the LV2 status codes (0x8001xxxx), so a system call's return compares equal
# to its name, and <errno.h> and <sys/return_code.h> agree.
#
# Checks, with a PPU compiler, in both data models and under -Werror:
#   - each of the 61 LV2 names equals its code (static assertions);
#   - <errno.h> then <sys/return_code.h>, and the reverse order, compile with
#     no redefinition; so do <sys/event.h> and <sys/synchronization.h>;
#   - names outside the LV2 table (ECHILD, ENOBUFS) keep small positive values.
# Red control: the same assertions against newlib's POSIX numbers (EBUSY 16)
# must fail, which proves the assertions bite.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: lv2-errno-values-test.sh [--ps3dev DIR]
set -u
ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "lv2-errno-values: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "lv2-errno-values: FAIL: no compiler at $cc"; exit 1; }
inc="-I$ps3dev/ppu/include"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

table="EAGAIN EINVAL ENOSYS ENOMEM ESRCH ENOENT ENOEXEC EDEADLK EPERM EBUSY
ETIMEDOUT EABORT EFAULT - ESTAT EALIGN EKRESOURCE EISDIR ECANCELED EEXIST
EISCONN ENOTCONN EAUTHFAIL ENOTMSELF ESYSVER EAUTHFATAL EDOM ERANGE EILSEQ
EFPOS EINTR EFBIG EMLINK ENFILE ENOSPC ENOTTY EPIPE EROFS ESPIPE E2BIG EACCES
EBADF EIO EMFILE ENODEV ENOTDIR ENXIO EXDEV EBADMSG EINPROGRESS EMSGSIZE
ENAMETOOLONG ENOLCK ENOTEMPTY ENOTSUP EFSSPECIFIC EOVERFLOW ENOTMOUNTED
ENOTSDATA ESDKVER ENOLICDISC ENOLICENT"

# Codes run 0x80010001 upward in table order; "-" marks the unused 0x8001000E.
{
    echo '#include <errno.h>'
    code=1
    for n in $table; do
        if [ "$n" != "-" ]; then
            printf '_Static_assert (%s == (int) 0x%08X, "%s");\n' "$n" $((0x80010000 + code)) "$n"
        fi
        code=$((code + 1))
    done
    echo '_Static_assert (ECHILD > 0 && ECHILD < 0x10000, "ECHILD keeps a POSIX value");'
    echo '_Static_assert (ENOBUFS > 0 && ENOBUFS < 0x10000, "ENOBUFS keeps a POSIX value");'
    echo '_Static_assert (EWOULDBLOCK == EAGAIN, "EWOULDBLOCK");'
} > "$work/values.c"
n=$(grep -c '0x8001' "$work/values.c")

printf '#include <errno.h>\n#include <sys/return_code.h>\nint x = EBUSY + CELL_OK;\n' > "$work/order1.c"
printf '#include <sys/return_code.h>\n#include <errno.h>\nint x = EBUSY + CELL_OK;\n' > "$work/order2.c"
printf '#include <sys/event.h>\n#include <sys/synchronization.h>\n#include <errno.h>\nint x = EBUSY;\n' > "$work/sync.c"

status=0
for model in "" -mlp64; do
    for f in values order1 order2 sync; do
        if "$cc" $model $inc -std=gnu11 -Wall -Werror -c "$work/$f.c" -o "$work/$f.o" 2> "$work/$f.err"; then
            echo "lv2-errno-values: ok   ${model:-ILP32} $f"
        else
            echo "lv2-errno-values: FAIL ${model:-ILP32} $f"; head -3 "$work/$f.err"; status=1
        fi
    done
done
[ "$n" = "61" ] || { echo "lv2-errno-values: FAIL table has $n codes, want 61"; status=1; }

# Red control: newlib's POSIX numbering (EBUSY 16) must fail the assertion.
printf '#define EBUSY 16
_Static_assert (EBUSY == (int) 0x8001000A, "EBUSY");
' > "$work/red.c"
if "$cc" -std=gnu11 -c "$work/red.c" -o "$work/red.o" 2>/dev/null; then
    echo "lv2-errno-values: FAIL red control: EBUSY 16 passed the assertion"; status=1
else
    echo "lv2-errno-values: ok   red control: a POSIX EBUSY fails the assertion"
fi

[ $status -eq 0 ] && echo "lv2-errno-values: PASS" || echo "lv2-errno-values: FAIL"
exit $status

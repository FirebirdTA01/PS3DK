#!/usr/bin/env bash
# cell/hash (libmd5, libsha1, libsha224, libsha256, libsha384, libsha512):
#  1. host: build sdk/libhash/src natively with hash-vectors-host.c and judge
#     every digest (28 lengths from 0 to 1000000 bytes, fed whole and in
#     1..13-byte pieces, and a reinitialised work area) against Python's
#     hashlib, plus the error-path contract (NULL pointers, update after
#     result, repeated result);
#  2. target, when PS3DEV is set: the sources and the headers compile with
#     the PPU compiler as gnu99, c11 and c++17, ILP32 and LP64, under
#     -Wall -Wextra -Werror.
# Self-check: one changed SHA-256 round constant must fail the sha224 and
# sha256 rows and no others.
#
# usage: hash-vectors-test.sh [--ps3dev DIR]
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
hostcc="${HOST_CC:-cc}"
python=$(command -v python3 || command -v python) || { echo "hash-vectors: FAIL: no python"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
src="$root/sdk/libhash/src"
# Only the hash headers: the rest of sdk/include wraps target libc headers.
mkdir -p "$work/inc/cell/hash"
cp "$root"/sdk/include/cell/hash/*.h "$work/inc/cell/hash/"
status=0

# judge <digest listing>: compare with hashlib; prints one line per algorithm.
judge() {
    "$python" - "$1" <<'PY'
import hashlib, sys
msg = bytearray(1000000)
x = 12345
for i in range(len(msg)):
    x = (x * 1103515245 + 12345) & 0xffffffff
    msg[i] = (x >> 16) & 0xff
msg = bytes(msg)
rows, bad, errs = {}, {}, []
for line in open(sys.argv[1]):
    f = line.split()
    if f[0] == 'ERR':
        errs.append(line.strip()); continue
    if f[0] == 'checks-failed':
        continue
    algo, n, how, got = f[0], int(f[1]), f[2], f[3]
    data = b'abc' if how == 'reinit-abc' else msg[:n]
    want = hashlib.new(algo, data).hexdigest()
    rows[algo] = rows.get(algo, 0) + 1
    if got != want:
        bad.setdefault(algo, []).append('%d/%s' % (n, how))
for algo in ['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512']:
    if algo not in rows:
        print('FAIL %s: no rows' % algo)
    elif algo in bad:
        print('FAIL %s: %d of %d digests wrong (first %s)' % (algo, len(bad[algo]), rows[algo], bad[algo][0]))
    else:
        print('ok   %s: %d digests' % (algo, rows[algo]))
for e in errs:
    print('FAIL contract: ' + e[4:])
PY
}

# host <label> <source-dir>: build and judge; output in $work/<label>.txt
host() {
    local label=$1 s=$2
    if ! "$hostcc" -std=c99 -O2 -Wall -Wextra -Werror -I"$work/inc" \
            "$s"/md5.c "$s"/sha1.c "$s"/sha224.c "$s"/sha256.c "$s"/sha384.c "$s"/sha512.c \
            "$root/tests/sdk/hash-vectors-host.c" -o "$work/$label.exe" 2>"$work/$label.err"; then
        echo "FAIL build" > "$work/$label.txt"; sed 's/^/    /' "$work/$label.err" | head -8 >> "$work/$label.txt"
        return
    fi
    "$work/$label.exe" > "$work/$label.out"
    judge "$work/$label.out" > "$work/$label.txt"
}

if command -v "$hostcc" >/dev/null 2>&1; then
    host real "$src"
    sed 's/^/hash-vectors: host /' "$work/real.txt"
    grep -q '^FAIL' "$work/real.txt" && status=1

    # Self-check: the last SHA-256 round constant changed in a copy of the
    # sources (a quoted include finds the copy's own sha256_core.h).
    cp -r "$src" "$work/mut"
    sed 's/0xbef9a3f7, 0xc67178f2,/0xbef9a3f7, 0xc67178f3,/' "$src/sha256_core.h" > "$work/mut/sha256_core.h"
    if cmp -s "$src/sha256_core.h" "$work/mut/sha256_core.h"; then
        echo "hash-vectors: FAIL self-check could not plant the changed constant"; status=1
    else
        host mut "$work/mut"
        if [ "$(grep '^FAIL' "$work/mut.txt" | awk '{print $2}' | tr -d : | sort | tr '\n' ' ')" = "sha224 sha256 " ]; then
            echo "hash-vectors: ok   self-check: a changed SHA-256 constant fails exactly sha224 and sha256"
        else
            echo "hash-vectors: FAIL self-check: wrong rows failed:"; sed 's/^/    /' "$work/mut.txt"; status=1
        fi
    fi
else
    echo "hash-vectors: FAIL: no host C compiler ($hostcc)"; status=1
fi

if [ -n "$ps3dev" ]; then
    cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
    if [ -x "$cc" ] || [ -x "$cc.exe" ]; then
        for abi in "" -mlp64; do
            if out=$(cd "$work" && "$cc" -std=gnu99 $abi -O2 -Wall -Wextra -Werror -I"$work/inc" -I"$src" -c \
                    "$src"/md5.c "$src"/sha1.c "$src"/sha224.c "$src"/sha256.c "$src"/sha384.c "$src"/sha512.c 2>&1); then
                echo "hash-vectors: ok   target sources ${abi:-ilp32}"
            else
                echo "hash-vectors: FAIL target sources ${abi:-ilp32}"; printf '%s\n' "$out" | grep error | head -5; status=1
            fi
            for lang in "c -std=gnu99" "c -std=c11" "c++ -std=c++17"; do
                set -- $lang
                if out=$(printf '#include <cell/hash/libmd5.h>\n#include <cell/hash/libsha1.h>\n#include <cell/hash/libsha224.h>\n#include <cell/hash/libsha256.h>\n#include <cell/hash/libsha384.h>\n#include <cell/hash/libsha512.h>\nint main(void){CellSha512WorkArea w; return cellSha512BlockInit(&w);}\n' |
                        "$cc" -x "$1" "$2" $abi -Wall -Wextra -Werror -pedantic -I"$work/inc" -c - -o "$work/h.o" 2>&1); then
                    echo "hash-vectors: ok   target headers $1 $2 ${abi:-ilp32}"
                else
                    echo "hash-vectors: FAIL target headers $1 $2 ${abi:-ilp32}"; printf '%s\n' "$out" | grep error | head -5; status=1
                fi
            done
        done
    else
        echo "hash-vectors: FAIL: no PPU compiler under $ps3dev"; status=1
    fi
else
    echo "hash-vectors: SKIP target leg (set PS3DEV or --ps3dev)"
fi

[ $status -eq 0 ] && echo "PASS: hash-vectors-test"
exit $status

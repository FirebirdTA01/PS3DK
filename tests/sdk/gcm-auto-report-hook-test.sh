#!/usr/bin/env bash
# The inline public report writers call the PS3_PROF_AUTO resource-write hook in an observed build and
# reference no hook in a plain build. Judged by exit status.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
flags="-Wall -Wextra -Werror -O2 -DATTRIBUTE_PRXPTR= -I$root/sdk/include -I$root/sdk/libgcm_cmd/include"
"$CC" -std=gnu11 $flags -DPS3TC_PROF_AUTO=1 "$root/tests/sdk/gcm-auto-report-hook-host.c" -o "$WORK/observed"
"$WORK/observed"
"$CC" -std=gnu11 $flags -c "$root/tests/sdk/gcm-auto-report-hook-host.c" -o "$WORK/plain.o"
if nm "$WORK/plain.o" | grep -q ps3dk_prof__auto_resource_write; then echo "FAIL plain build references the hook"; exit 1; fi
"$CC" "$WORK/plain.o" -o "$WORK/plain"
"$WORK/plain"
echo "gcm-auto-report-hook: PASS"

#!/usr/bin/env bash
# Build and run both variants of the host reproduction. Self-contained: needs
# only gcc and pthreads, no PS3 toolchain and no emulator.
#
#   signal    variant -> expected to deadlock (timeout, exit 124)
#   broadcast variant -> expected to pass     (exit 0, prints HOST_OK)
#
# If the signal variant hangs here too, the bug under investigation is in the
# sample's synchronization, not in the librt pthread shim. See README.md.
set -u
cd "$(dirname "$0")" || exit 1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "compiler: $(gcc --version | head -1)"
echo "libc:     $(ldd --version 2>/dev/null | head -1)"

gcc -O2 -o "$work/repro_signal" hostrepro.c -lpthread || exit 1
gcc -O2 -DUSE_BROADCAST -o "$work/repro_bcast" hostrepro.c -lpthread || exit 1

status=0
for variant in signal bcast; do
	echo "===== variant: $variant"
	for i in 1 2 3 4 5; do
		timeout 5 stdbuf -oL "$work/repro_$variant" > "$work/out.$variant.$i" 2>&1
		rc=$?
		printf '  run %d: exit=%-4s last=%s\n' \
			"$i" "$rc" "$(tail -1 "$work/out.$variant.$i")"
		# 124 is the expected verdict for the signal variant and a failure for
		# the broadcast one; anything else either way is worth looking at.
		case "$variant:$rc" in
			signal:124|bcast:0) ;;
			*) status=1 ;;
		esac
	done
done

echo "===== full output, signal variant, run 1"
cat "$work/out.signal.1"

if [[ $status -eq 0 ]]; then
	echo "VERDICT: as expected - cond_signal deadlocks, cond_broadcast passes."
else
	echo "VERDICT: UNEXPECTED - read the runs above before drawing conclusions." >&2
fi
exit $status

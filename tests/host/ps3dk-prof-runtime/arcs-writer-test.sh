#!/usr/bin/env bash
# Host only. Caller holds its normal build slot. READER is the candidate host tool.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:?usage: arcs-writer-test.sh OUTPUT_DIR}"
reader="${READER:?READER must name the built candidate ps3dk-prof}"
mkdir -p "$out"
for variant in plain asan; do
    flags=()
    if [ "$variant" = asan ]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    "${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
        -I"$root/sdk/libps3dkprof/src/core" "$here/arcs_writer_test.c" \
        "$root/sdk/libps3dkprof/src/core/prof_arcs_writer.c" -o "$out/writer-$variant"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/writer-$variant" "$out/$variant.arcs" >"$out/$variant.log" 2>"$out/$variant.err"
    tail -n 1 "$out/$variant.log"
    python3 "$here/arcs_writer_oracle.py" "$out/$variant.arcs"
    "$reader" check-arcs "$out/$variant.arcs"
    "$reader" arcs-json "$out/$variant.arcs" >"$out/$variant.json"
done

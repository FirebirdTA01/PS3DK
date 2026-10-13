#!/usr/bin/env bash
# A PPU header (sdk/include) with the same path as an SPU header
# (sdk/include-spu) shadows it in every in-tree SPU library that lists
# sdk/include first (libspurs_task, libspuPSGL).  v0.24's <cell/sync/*.h>
# forwarders did exactly that and broke the SDK build in the release
# pipeline, which CI does not run.  Host-only check, no compiler needed:
#  - a same-path pair listed in spu-header-shadow-allowlist.txt predates the
#    rule (each one was written for both trees and is left alone);
#  - any other pair must send SPU compiles on with `#include_next` inside an
#    `#ifdef __SPU__` block, so the SPU header is the one an SPU build reads.
# Self-check: a planted pair without #include_next must fail, and the same
# pair with the __SPU__ branch must pass.
#
# usage: spu-header-shadow-test.sh [ROOT]
set -u
root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)}"
allow="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/spu-header-shadow-allowlist.txt"

check() {  # <root> -> prints offending paths, returns their count
    local r="$1" bad=0 rel
    while IFS= read -r rel; do
        [ -f "$r/sdk/include-spu/$rel" ] || continue
        grep -qxF "$rel" "$allow" && continue
        if ! awk '/^#[ \t]*ifdef[ \t]+__SPU__/{s=1} s && /^#[ \t]*include_next/{ok=1} /^#[ \t]*(else|endif)/{s=0} END{exit !ok}' "$r/sdk/include/$rel"; then
            echo "  $rel shadows sdk/include-spu/$rel without an __SPU__ #include_next"
            bad=$((bad + 1))
        fi
    done < <(cd "$r/sdk/include" && find . -name '*.h' | sed 's#^\./##' | LC_ALL=C sort)
    return "$bad"
}

status=0
out=$(check "$root"); n=$?
if [ "$n" -eq 0 ]; then
    echo "spu-header-shadow: ok   no unguarded PPU/SPU same-path header"
else
    echo "spu-header-shadow: FAIL $n unguarded same-path header(s):"; echo "$out"; status=1
fi

# Self-check on a synthetic tree.
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$work/sdk/include/cell/zz" "$work/sdk/include-spu/cell/zz"
echo '/* spu */' > "$work/sdk/include-spu/cell/zz/planted.h"
printf '%s\n' '#include <cell/zz.h>' > "$work/sdk/include/cell/zz/planted.h"
if check "$work" > /dev/null; then
    echo "spu-header-shadow: FAIL self-check: an unguarded planted pair passed"; status=1
else
    echo "spu-header-shadow: ok   self-check: an unguarded planted pair fails"
fi
printf '%s\n' '#ifdef __SPU__' '#include_next <cell/zz/planted.h>' '#else' '#include <cell/zz.h>' '#endif' \
    > "$work/sdk/include/cell/zz/planted.h"
if check "$work" > /dev/null; then
    echo "spu-header-shadow: ok   self-check: the __SPU__ #include_next form passes"
else
    echo "spu-header-shadow: FAIL self-check: the guarded form was rejected"; status=1
fi

[ $status -eq 0 ] && echo "PASS: spu-header-shadow-test"
exit $status

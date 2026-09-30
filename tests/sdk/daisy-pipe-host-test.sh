#!/usr/bin/env bash
# Host test for the PPU libdaisy ports (tests/sdk/daisy-pipe-host-test.cpp):
# cancelled pushes and pops come back on the port's next begin while other
# ends hold later entries, and nothing is rolled back in the queue.
#
# Also checks the compile-time contract: the data forms of begin / push / pop
# need a COPY port, the no-data forms and getCurrentReference() a REFERENCE
# port, and cancelling is refused for COPY ports on QueueControl::Local.
# Each refusal must fail to compile with its own message (C++17 and C++98),
# and a control using every form correctly must compile.
#
# usage: daisy-pipe-host-test.sh     (CXX overrides the host compiler)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cxx="${CXX:-c++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
inc=(-I"$root/tests/sdk/fixtures/daisy-ppu-host" -I"$root/sdk/include")

"$cxx" -std=c++17 -O2 -Wall -Wextra -Werror "${inc[@]}" \
    "$root/tests/sdk/daisy-pipe-host-test.cpp" -o "$work/daisy-pipe" -pthread
"$work/daisy-pipe"

# name | port declaration | statement | expected message fragment
cases=(
"copy-push-nodata|Pipe::InPort<Buf, In> p(buf, qi);|p.beginPush();|needs a REFERENCE port"
"copy-trypush-nodata|Pipe::InPort<Buf, In> p(buf, qi);|p.tryBeginPush();|needs a REFERENCE port"
"ref-push-data|Pipe::InPort<Buf, In, REFERENCE> p(buf, qi);|p.push(&w);|needs a COPY port"
"ref-beginpush-data|Pipe::InPort<Buf, In, REFERENCE> p(buf, qi);|p.beginPush(&w);|needs a COPY port"
"copy-pop-nodata|Pipe::OutPort<Buf, Out> p(buf, qo);|p.beginPop();|needs a REFERENCE port"
"ref-pop-data|Pipe::OutPort<Buf, Out, REFERENCE> p(buf, qo);|p.pop(&w);|needs a COPY port"
"copy-getref|Pipe::InPort<Buf, In> p(buf, qi);|p.getCurrentReference();|needs a REFERENCE port"
"copy-local-cancelpush|Pipe::InPort<Buf, In> p(buf, qi);|p.cancelPush();|cancel is not available"
"copy-local-cancelpop|Pipe::OutPort<Buf, Out> p(buf, qo);|p.cancelPop();|cancel is not available"
)

snippet() {  # $1 declaration, $2 statement
    cat <<EOF
#include <cell/daisy.h>
using namespace cell::Daisy;
struct Word { uint32_t v, pad[3]; };
typedef Buffer::Local<Word, 4> Buf;
typedef QueueControl::Local<4, INPUT> In;
typedef QueueControl::Local<4, OUTPUT> Out;
static Lock lock __attribute__((aligned(128)));
int main()
{
	Buf buf;
	In qi(lock);
	Out qo(lock);
	Word w = { 0, {0, 0, 0} };
	(void)w; (void)qo; (void)qi;
	$1
	$2
	return 0;
}
EOF
}

for std in c++17 c++98; do
    # control: every form on the port it belongs to
    snippet "Pipe::InPort<Buf, In> pc(buf, qi); Pipe::InPort<Buf, In, REFERENCE> pr(buf, qi); Pipe::OutPort<Buf, Out> oc(buf, qo); Pipe::OutPort<Buf, Out, REFERENCE> orf(buf, qo);" \
        "pc.tryBeginPush(&w); pc.push(&w); pr.tryBeginPush(); pr.getCurrentReference(); pr.cancelPush(); oc.tryBeginPop(&w); orf.tryBeginPop(); orf.cancelPop();" \
        > "$work/control.cpp"
    "$cxx" -std=$std -fsyntax-only -Wall -Wextra -Werror "${inc[@]}" "$work/control.cpp"
    for c in "${cases[@]}"; do
        IFS='|' read -r name decl stmt want <<< "$c"
        snippet "$decl" "$stmt" > "$work/$name.cpp"
        set +e
        "$cxx" -std=$std -fsyntax-only "${inc[@]}" "$work/$name.cpp" > "$work/$name.log" 2>&1
        rc=$?
        set -e
        if [ "$rc" -ne 1 ]; then
            echo "FAIL: $name ($std) compiled or crashed (exit $rc), expected a refusal"
            cat "$work/$name.log"
            exit 1
        fi
        # C++98 has no static_assert message; the refusal names the check
        if [ "$std" = c++17 ] && ! grep -q "$want" "$work/$name.log"; then
            echo "FAIL: $name ($std) refused for another reason:"
            cat "$work/$name.log"
            exit 1
        fi
        if [ "$std" = c++98 ] && ! grep -q "ps3tcDaisyRequire" "$work/$name.log"; then
            echo "FAIL: $name ($std) refused for another reason:"
            cat "$work/$name.log"
            exit 1
        fi
    done
    echo "daisy-pipe: $std control compiles, ${#cases[@]} misuses refused"
done
echo "daisy-pipe-host-test: PASS"

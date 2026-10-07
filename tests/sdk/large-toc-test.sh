#!/usr/bin/env bash
# A compact-.opd link that needs more than one TOC group is refused
# (binutils patch 0005).  Past 64 KiB of TOC reached by small-model TOC
# references, ld splits the TOC into groups and enters other groups
# through stubs that save r2 with std and restore it with ld, while
# hybrid ILP32 code keeps its saved TOC in the word at 40(r1) (stw/lwz):
# the stub's std zeroes that word and the next lwz restore loads r2 = 0.
#
# Per ABI, from one generated fixture (code_J.c reaches NV extern ints
# defined in data_J.c through the TOC):
#   big   -mcmodel=small, 4 objects (> 64 KiB of TOC): gcc exits 1 with
#         ld's "needs multiple TOC groups" diagnostic and writes no ELF.
#         An unpatched ld links it with several TOC groups (the checker
#         prints how many descriptor TOC words it found).
#   ctl   -mcmodel=small, 2 objects (< 64 KiB): links, one TOC group.
#   med   the big object set at the default (medium) code model: links,
#         one TOC group - the refusal never touches default builds.
# Before the real links, the refusal judge is run on doctored results
# (exit 1 with an unrelated error, exit 124 with the right text, exit 1
# naming the token only in a path, exit 0 with an ELF) and must reject
# every one with exit 1.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: large-toc-test.sh [--ps3dev DIR] [-B DIR]
#   -B DIR is passed to the driver, e.g. a directory holding a candidate ld.
set -u
ps3dev="${PS3DEV:-}"; bdir=""
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        -B) bdir="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [-B DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "large-toc: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "large-toc: FAIL: no compiler at $cc"; exit 1; }
py=python3; command -v $py > /dev/null 2>&1 || py=python
B=(); [ -n "$bdir" ] && B=("-B$bdir")
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The diagnostic body, matched as ld's own "<prog>: error: ..." line.
DIAG=': error: Cell OS Lv-2 compact \.opd link needs multiple TOC groups \(over 64 KiB of TOC'

cat > "$work/gen.py" <<'EOF'
import os, sys
out, nobj, nv = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
os.makedirs(out, exist_ok=True)
for j in range(nobj):
    with open(os.path.join(out, "data_%d.c" % j), "w") as f:
        for i in range(nv):
            f.write("int v_%d_%d = %d;\n" % (j, i, j * nv + i + 1))
    with open(os.path.join(out, "code_%d.c" % j), "w") as f:
        for i in range(nv):
            f.write("extern volatile int v_%d_%d;\n" % (j, i))
        f.write("unsigned f_%d(void)\n{\n    unsigned s = 0;\n" % j)
        for i in range(nv):
            f.write("    s += (unsigned)v_%d_%d;\n" % (j, i))
        f.write("    return s;\n}\n")
# main<N>.c calls f_0 .. f_<N-1> directly and through their descriptors.
for n in (2, nobj):
    with open(os.path.join(out, "main%d.c" % n), "w") as f:
        for j in range(n):
            f.write("extern unsigned f_%d(void);\n" % j)
        f.write("unsigned (*volatile fns[])(void) = {%s};\n"
                % ", ".join("f_%d" % j for j in range(n)))
        f.write("int main(void)\n{\n    unsigned s = 0;\n")
        for j in range(n):
            f.write("    s += f_%d() + fns[%d]();\n" % (j, j))
        f.write("    return (int)(s & 1);\n}\n")
EOF

# Distinct TOC words over every compact descriptor in .opd.
cat > "$work/tocwords.py" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
    print("REFUSE: not a big-endian ELF64"); sys.exit(2)
shoff, = struct.unpack_from(">Q", d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from(">HHH", d, 0x3a)
secs = [struct.unpack_from(">IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
strs = secs[shstrndx]
def name(s): o = strs[4] + s[0]; return d[o:d.index(b"\0", o)].decode()
opd = [s for s in secs if name(s) == ".opd"]
if len(opd) != 1 or opd[0][8] != 4 or opd[0][5] % 8:
    print("REFUSE: no compact .opd (4-aligned, 8-byte entries)"); sys.exit(2)
off, size = opd[0][4], opd[0][5]
words = sorted({struct.unpack_from(">I", d, off + k + 4)[0] for k in range(0, size, 8)})
print("toc-words %d %s" % (len(words), " ".join("0x%x" % w for w in words)))
EOF

status=0
ok() { echo "large-toc: ok   $*"; }
fail() { echo "large-toc: FAIL $*"; status=1; }

# judge_refusal RC LOG ELF -> 0 when the link was refused the way patch
# 0005 refuses it: exit status exactly 1, ld's diagnostic line, no ELF.
judge_refusal() {
    [ "$1" -eq 1 ] || { echo "exit $1, expected 1"; return 1; }
    grep -Eq -- "$DIAG" "$2" || { echo "no '$DIAG' line"; return 1; }
    [ ! -s "$3" ] || { echo "an ELF was written"; return 1; }
    return 0
}

# Self-check: every doctored result must be rejected.
mkdir -p "$work/judge"
: > "$work/judge/none.elf"
printf 'ld.exe: error: some other failure\ncollect2.exe: error: ld returned 1 exit status\n' > "$work/judge/unrelated.log"
printf 'ld.exe: error: Cell OS Lv-2 compact .opd link needs multiple TOC groups (over 64 KiB of TOC reached\n' > "$work/judge/right.log"
printf 'ld.exe: cannot open /x/Cell OS Lv-2 compact .opd link needs multiple TOC groups (over 64 KiB of TOC.o\n' > "$work/judge/inpath.log"
printf '\177ELF' > "$work/judge/some.elf"
for c in "unrelated|1|unrelated.log|none.elf" "timeout|124|right.log|none.elf" \
         "token-in-path|1|inpath.log|none.elf" "elf-written|1|right.log|some.elf" \
         "exit-0|0|right.log|none.elf"; do
    IFS='|' read -r what rc log elf <<< "$c"
    if judge_refusal "$rc" "$work/judge/$log" "$work/judge/$elf" > /dev/null; then
        fail "judge self-check $what: accepted a result it must reject"
    else
        ok "judge self-check $what: rejected"
    fi
done
judge_refusal 1 "$work/judge/right.log" "$work/judge/none.elf" > /dev/null \
    && ok "judge self-check genuine: accepted" || fail "judge self-check genuine: rejected the real shape"

compile() {  # abi dir model obj...
    local abi=$1 dir=$2 model=$3; shift 3
    local fl=(); [ "$abi" = lp64 ] && fl=(-mlp64)
    local o
    for o in "$@"; do
        if ! "$cc" "${fl[@]}" $model -O2 -c "$dir/$o.c" -o "$dir/$o$model.o" > "$dir/$o$model.log" 2>&1; then
            fail "$abi: $o.c ($model) does not compile"; cat "$dir/$o$model.log"; return 1
        fi
    done
}

for spec in "ilp32 6000" "lp64 3000"; do
    read -r abi nv <<< "$spec"
    fl=(); [ "$abi" = lp64 ] && fl=(-mlp64)
    dir="$work/$abi"
    "$py" "$work/gen.py" "$dir" 4 "$nv" || { fail "$abi: generator failed"; continue; }
    compile "$abi" "$dir" -mcmodel=small main2 main4 code_0 code_1 code_2 code_3 || continue
    compile "$abi" "$dir" -mcmodel=medium main4 code_0 code_1 code_2 code_3 || continue
    compile "$abi" "$dir" "" data_0 data_1 data_2 data_3 || continue

    link() {  # name model nobj
        local objs=("$dir/main$3$2.o") j
        for ((j = 0; j < $3; j++)); do objs+=("$dir/code_$j$2.o" "$dir/data_$j.o"); done
        rm -f "$dir/$1.elf"
        "$cc" "${B[@]}" "${fl[@]}" "${objs[@]}" -o "$dir/$1.elf" > "$dir/$1.log" 2>&1
    }

    # big: refused.
    link big -mcmodel=small 4; rc=$?
    if why=$(judge_refusal $rc "$dir/big.log" "$dir/big.elf"); then
        ok "$abi big (-mcmodel=small, 4 objects): refused with ld's multiple-TOC-groups error"
    else
        fail "$abi big (-mcmodel=small, 4 objects): $why"
        [ -s "$dir/big.elf" ] && "$py" "$work/tocwords.py" "$dir/big.elf"
        cat "$dir/big.log"
    fi

    # ctl and med: link, one TOC group, no diagnostic.
    for c in "ctl|-mcmodel=small|2" "med|-mcmodel=medium|4"; do
        IFS='|' read -r name model n <<< "$c"
        link "$name" "$model" "$n"; rc=$?
        words=$("$py" "$work/tocwords.py" "$dir/$name.elf" 2>&1)
        if [ $rc -eq 0 ] && [ ! -s "$dir/$name.log" ] && [ "${words%% *}" = toc-words ] \
                && [ "$(echo "$words" | cut -d' ' -f2)" = 1 ]; then
            ok "$abi $name ($model, $n objects): links, one TOC group"
        else
            fail "$abi $name ($model, $n objects): exit $rc, $words"; cat "$dir/$name.log"
        fi
    done
done

[ $status -eq 0 ] && echo "large-toc: PASS" || echo "large-toc: FAIL"
exit $status

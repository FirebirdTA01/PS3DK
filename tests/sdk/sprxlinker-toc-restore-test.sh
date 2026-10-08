#!/usr/bin/env bash
# sprxlinker gives every call to an LP64 import stub its TOC restore, with or
# without --lp64, and never touches a call to an ILP32 stub.
#
# An LP64 import stub saves r2 and tail-calls the export (std r2,40(r1) ...
# bctr), so the caller must reload its TOC: the nop after the bl has to become
# ld r2,40(r1).  That rewrite used to run only under --lp64, which Make's
# ppu_rules never passes; an LP64 ELF packed without it returned from its
# first import with the firmware's TOC in r2 and faulted on the next
# TOC-relative access.  An ILP32 stub calls the export (bctrl) and restores r2
# itself, so its callers must be left alone; --lp64 on an ILP32 ELF used to
# rewrite them to an 8-byte load of a 4-byte save slot.
#
# The fixtures are small hand-made ELF64 files (no PPU toolchain needed):
#   mixed  calls to an LP64 stub and an ILP32 stub, plus one call already
#          restored.  Without --lp64 and with it: only the LP64 call's nop
#          changes.  A second run changes nothing.
#   ilp32  calls to the ILP32 stub only: byte-identical after the run.
#   noslot an LP64 call followed by an instruction that is not a nop, or in
#          the last word of .text (an ILP32 call there is left alone): exit 1,
#          the call's address named.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
sprxlinker="${1:-$repo_root/tools/sprx-linker/sprxlinker}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$sprxlinker" ]] || fail "sprxlinker not executable: $sprxlinker"
command -v python3 > /dev/null || fail "python3 not found"

work="$(mktemp -d "${TMPDIR:-/tmp}/sprxlinker-toc.XXXXXX")"
trap 'rm -rf "$work"' EXIT

python3 - "$work" << 'EOF'
import struct, sys
work = sys.argv[1]
TEXT, STUBS = 0x10000, 0x20000
NOP, LD_R2, BLR = 0x60000000, 0xe8410028, 0x4e800020
# LP64 stub: save r2, load the export's descriptor, tail-call it.
LP64_STUB = [0xf8410028, 0x3d800002, 0x818c0100, 0x800c0000,
             0x804c0004, 0x7c0903a6, 0x4e800420]
# ILP32 stub: call the export and restore r2 before returning.
ILP32_STUB = [0x7c0802a6, 0x90010018, 0x90410028, 0x3d800002, 0x818c0104,
              0x800c0000, 0x804c0004, 0x7c0903a6, 0x4e800421, 0x80410028,
              0x80010018, 0x7c0803a6, 0x4e800020]
LP64_AT = STUBS
ILP32_AT = STUBS + 4 * len(LP64_STUB)

def bl(pc, target):
    return 0x48000001 | ((target - pc) & 0x03fffffc)

def write_elf(path, text):
    stubs = LP64_STUB + ILP32_STUB
    tb = b"".join(struct.pack(">I", w) for w in text)
    sb = b"".join(struct.pack(">I", w) for w in stubs)
    # .sys_proc_prx_param: size 0x40 and the magic, as the link writes it.
    pb = struct.pack(">II", 0x40, 0x1b434cec) + bytes(56)
    shstr = b"\0.text\0.sceStub.text\0.shstrtab\0.sys_proc_prx_param\0"
    off_text = 64
    off_stub = off_text + len(tb)
    off_prx = off_stub + len(sb)
    off_str = off_prx + len(pb)
    off_sh = (off_str + len(shstr) + 7) & ~7
    eh = struct.pack(">16sHHIQQQIHHHHHH",
                     b"\x7fELF\x02\x02\x01" + b"\0" * 9, 2, 21, 1, TEXT,
                     0, off_sh, 0, 64, 0, 0, 64, 5, 3)
    def sh(name, typ, flags, addr, off, size, align):
        return struct.pack(">IIQQQQIIQQ", name, typ, flags, addr, off, size, 0, 0, align, 0)
    shs = (sh(0, 0, 0, 0, 0, 0, 0)
           + sh(1, 1, 6, TEXT, off_text, len(tb), 4)
           + sh(7, 1, 6, STUBS, off_stub, len(sb), 4)
           + sh(21, 3, 0, 0, off_str, len(shstr), 1)
           + sh(31, 1, 2, 0x30000, off_prx, len(pb), 4))
    data = eh + tb + sb + pb + shstr
    data += b"\0" * (off_sh - len(data)) + shs
    open(path, "wb").write(data)

def text_of(calls):
    words = []
    for target, after in calls:
        words += [bl(TEXT + 4 * len(words), target), after]
    return words + [BLR]

# mixed: LP64 call + nop, ILP32 call + nop, LP64 call already restored.
write_elf(work + "/mixed.elf", text_of([(LP64_AT, NOP), (ILP32_AT, NOP), (LP64_AT, LD_R2)]))
write_elf(work + "/mixed.want", text_of([(LP64_AT, LD_R2), (ILP32_AT, NOP), (LP64_AT, LD_R2)]))
write_elf(work + "/ilp32.elf", text_of([(ILP32_AT, NOP), (ILP32_AT, NOP)]))
# noslot: the second LP64 call is followed by ori r1,r1,0, not a nop.
write_elf(work + "/noslot.elf", text_of([(LP64_AT, NOP), (LP64_AT, 0x60210000)]))
# The call is the last word of .text: no slot after it at all.
write_elf(work + "/lastlp64.elf", [BLR, bl(TEXT + 4, LP64_AT)])
write_elf(work + "/lastilp32.elf", [BLR, bl(TEXT + 4, ILP32_AT)])
EOF

run() {  # <fixture> <out> [flag]; prints the exit status
    local rc=0
    cp "$work/$1.elf" "$work/$2"
    "$sprxlinker" ${3:-} "$work/$2" > "$work/$2.log" 2>&1 || rc=$?
    printf '%s' "$rc"
}

# ---- mixed, without --lp64 and with it -----------------------------------
for flag in "" "--lp64"; do
    rc="$(run mixed "mixed${flag}.out" "$flag")"
    [[ "$rc" -eq 0 ]] || fail "mixed ${flag:-(no flag)}: exit $rc ($(head -1 "$work/mixed${flag}.out.log"))"
    cmp -s "$work/mixed${flag}.out" "$work/mixed.want" \
        || fail "mixed ${flag:-(no flag)}: only the LP64 call's nop may change, to ld r2,40(r1) ($(cmp "$work/mixed${flag}.out" "$work/mixed.want" | head -1))"
    printf '  ok: mixed %s: LP64 call restored, ILP32 call and the restored call untouched\n' "${flag:-(no flag)}"
done

# ---- a second run changes nothing ------------------------------------------
cp "$work/mixed.out" "$work/mixed.again"
"$sprxlinker" "$work/mixed.again" > "$work/again.log" 2>&1 || fail "second run: exit $? ($(head -1 "$work/again.log"))"
cmp -s "$work/mixed.again" "$work/mixed.want" || fail "a second run changed the file"
printf '  ok: a second run changes nothing\n'

# ---- ilp32: byte-identical, with --lp64 too --------------------------------
for flag in "" "--lp64"; do
    rc="$(run ilp32 "ilp32${flag}.out" "$flag")"
    [[ "$rc" -eq 0 ]] || fail "ilp32 ${flag:-(no flag)}: exit $rc ($(head -1 "$work/ilp32${flag}.out.log"))"
    cmp -s "$work/ilp32${flag}.out" "$work/ilp32.elf" \
        || fail "ilp32 ${flag:-(no flag)}: a call to an ILP32 stub was rewritten"
    printf '  ok: ilp32 %s: byte-identical\n' "${flag:-(no flag)}"
done

# ---- noslot: refused, the call named ---------------------------------------
rc="$(run noslot noslot.out)"
[[ "$rc" -eq 1 ]] || fail "noslot: expected exit 1, got $rc"
grep -qF 'import call at 0x10008' "$work/noslot.out.log" \
    || fail "noslot: expected the call at 0x10008 named, got $(head -1 "$work/noslot.out.log")"
printf '  ok: noslot refused, call at 0x10008 named\n'

# ---- a call in the last word of .text --------------------------------------
rc="$(run lastlp64 lastlp64.out)"
[[ "$rc" -eq 1 ]] || fail "lastlp64: expected exit 1, got $rc"
grep -qF 'import call at 0x10004 is the last word' "$work/lastlp64.out.log" \
    || fail "lastlp64: expected the call at 0x10004 named, got $(head -1 "$work/lastlp64.out.log")"
printf '  ok: an LP64 call in the last word refused, call at 0x10004 named\n'
rc="$(run lastilp32 lastilp32.out)"
[[ "$rc" -eq 0 ]] || fail "lastilp32: exit $rc ($(head -1 "$work/lastilp32.out.log"))"
cmp -s "$work/lastilp32.out" "$work/lastilp32.elf" || fail "lastilp32: an ILP32 call in the last word was changed"
printf '  ok: an ILP32 call in the last word is byte-identical\n'

echo "sprxlinker-toc-restore: PASS"

#!/usr/bin/env bash
# Path-independent SPU embeds.
#
# WHAT IT FIXES / PINS
# ps3_add_spu_image (cmake/ps3-self.cmake) links the SPU ELF with an ABSOLUTE
# -o path.  The SPU linker writes the -o value verbatim into a diagnostic
# NOTE section .note.spu_name (name "SPUNAME\0", type 1, desc = the -o value,
# null-padded).  spurs-job-entry-point.md section 2.1 specifies the desc as
# the padded basename, and the note is embedded VERBATIM (bin2s) into the PPU
# executable, so the host's build path ended up inside every PPU binary that
# carries the image.  Two builds of the same sample in build directories of
# equal path length produced PPU ELFs that differed only inside that desc.
#
# THE FIX
# The link runs in the image's directory with -o the bare file name, so the
# note records just "<name>.bin" and the image no longer depends on where
# the build ran.
#
# THE TEST
# 1. Build the real spurs sample (samples/spurs/hello-spurs-task) twice, in
#    two build directories of EQUAL NAME LENGTH but DIFFERENT NAMES (aa / bb),
#    redirecting each PPU ELF into its own build dir.
# 2. Assert the two final PPU ELFs (hello-spurs-task.elf) are byte-identical,
#    and so are the SPU images embedded in them.
# 3. Assert the .note.spu_name desc in the embedded image is exactly the bare
#    "spu_task.bin", i.e. not either build dir (pins spurs-job-entry-point.md
#    section 2.1).
# 4. Control: the same SPU program linked with an absolute -o records the
#    absolute path, and with a bare -o the bare name, so (2) and (3) cannot
#    pass merely because the note went missing.
#
# SKIPS when the cross toolchain is absent (host toolchain-less CI).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
ps3dev="${PS3DEV:-}"
if [ -z "$ps3dev" ] && [ -d "$repo_root/stage/ps3dev" ]; then
    ps3dev="$repo_root/stage/ps3dev"
fi

spucc=""
[ -n "$ps3dev" ] && [ -x "$ps3dev/spu/bin/spu-elf-gcc" ] && spucc="$ps3dev/spu/bin/spu-elf-gcc"

sample="$repo_root/samples/spurs/hello-spurs-task"

skip() { printf 'spu-name-note-path: SKIP (%s)\n' "$1"; exit 0; }
[ -n "$spucc" ] || skip "PS3DEV spu toolchain not found"
[ -f "$sample/CMakeLists.txt" ] || skip "sample missing: $sample"

if ! command -v cmake >/dev/null 2>&1; then
    skip "cmake not found"
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "python3 not found"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/spu-name-note-path.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# Note-desc extractor: prints the .note.spu_name desc (trailing NULs stripped)
# of an elf32-spu image.  Extract in Python, not readelf (per task spec — it
# must work regardless of which readelf/objdump host flavor is present).
extract_note() {  # <spu-elf-or-bin>
    python3 - "$1" <<'PY'
import sys, struct
p = sys.argv[1]
d = open(p, "rb").read()
if d[:4] != b"\x7fELF":
    sys.exit(3)
# elf32-spu is big-endian.  e_shoff at 0x20; e_shentsize, e_shnum and
# e_shstrndx at 0x2e, 0x30 and 0x32.
if d[4] != 1 or d[5] != 2:
    sys.exit(3)
(e_shoff,) = struct.unpack_from(">I", d, 0x20)
(e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(">HHH", d, 0x2e)
secs = []
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    fields = struct.unpack_from(">IIIIII", d, o)
    secs.append((fields[1], fields[4], fields[5], fields[0]))  # type,off,size,name
sof, ssz = secs[e_shstrndx][1], secs[e_shstrndx][2]
def nm(n):
    end = d.index(b"\0", sof + n)
    return d[sof + n : end].decode("ascii", "replace")
for t, off, size, name in secs:
    if t == 7 and nm(name) == ".note.spu_name":
        namesz, descsz, ntype = struct.unpack_from(">III", d, off)
        de = off + 12 + ((namesz + 3) & ~3)
        desc = d[de : de + descsz]
        i = desc.find(b"\0"); i = len(desc) if i < 0 else i
        sys.stdout.buffer.write(desc[:i])
        sys.exit(0)
sys.exit(2)
PY
}

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

build_one() {  # <build-dir>; writes "<ppu-elf> <spu-image>" to <build-dir>.paths
    local bld="$1"
    local out="$bld/out"
    mkdir -p "$out"
    # Redirect the post-build PPU ELF/.self into this build's own dir so the
    # two builds (same sample, same CMAKE_CURRENT_SOURCE_DIR) do not collide.
    cmake -S "$sample" -B "$bld" \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake/ps3-ppu-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPS3_SELF_OUTPUT_DIRECTORY="$out" \
        > "$bld.configure.log" 2>&1 \
        || fail "configure failed in $bld ($(tail -n 5 "$bld.configure.log"))"
    cmake --build "$bld" > "$bld/build.log" 2>&1 \
        || fail "build failed in $bld ($(tail -n 25 "$bld/build.log"))"
    local ppu spu
    ppu="$out/hello-spurs-task.elf"
    [ -f "$ppu" ] || fail "PPU ELF hello-spurs-task.elf not produced under $out"
    spu="$(find "$bld" -path '*/spu/spu_task/spu_task.bin' -type f | head -n1)"
    [ -n "$spu" ] && [ -f "$spu" ] || fail "spu_task.bin not produced under $bld"
    printf '%s %s
' "$ppu" "$spu" > "$bld.paths"
}

# --- (1)+(2)+(3): two equal-length, different build dirs, byte-identical ---
ROOT="$work/root"
mkdir -p "$ROOT/aa" "$ROOT/bb"   # "aa" and "bb": equal length (2), different
build_one "$ROOT/aa"
build_one "$ROOT/bb"
read -r PPU_A SPU_A < "$ROOT/aa.paths"
read -r PPU_B SPU_B < "$ROOT/bb.paths"

# (2a) The final PPU ELFs must be byte-identical — this is the artifact that
#      differs between host builds when the path leaks.
PA="$(sha256sum "$PPU_A" | cut -d' ' -f1)"
PB="$(sha256sum "$PPU_B" | cut -d' ' -f1)"
[ "$PA" = "$PB" ] || {
    fail "PPU ELFs differ at equal-length build dirs: $PA vs $PB"
}
printf '  byte-identical PPU ELF across build dirs (hello-spurs-task.elf): %s\n' "$PA"

# (2b) The embedded SPU image inside them is identical too (the image is what
#      carried the path; a stronger, independent check).
SA="$(sha256sum "$SPU_A" | cut -d' ' -f1)"
SB="$(sha256sum "$SPU_B" | cut -d' ' -f1)"
[ "$SA" = "$SB" ] || fail "embedded SPU images differ: $SA vs $SB"
printf '  byte-identical embedded SPU image (spu_task.bin): %s\n' "$SA"

# (3) The note desc in the embedded image is exactly the bare file name.
DA="$(extract_note "$SPU_A")"
DB="$(extract_note "$SPU_B")"
[ "$DA" = "spu_task.bin" ] || fail "build A .note.spu_name desc = '$DA', want 'spu_task.bin'"
[ "$DB" = "spu_task.bin" ] || fail "build B .note.spu_name desc = '$DB', want 'spu_task.bin'"
printf '  .note.spu_name desc == "spu_task.bin" in both (spurs-job-entry-point.md section 2.1)\n'

# --- (4) control: the linker records -o as given ----------------------------
# The same SPU program linked twice: with an absolute -o the note holds the
# absolute path (the old rule's leak), with a bare -o it holds the bare name.
# Without this row, (2) and (3) could pass because the note had vanished.
mkdir -p "$work/ctrl"
printf 'int main(void) { return 0; }
' > "$work/ctrl/ctrl.c"
"$spucc" -c -o "$work/ctrl/ctrl.o" "$work/ctrl/ctrl.c" || fail "control: SPU compile failed"
"$spucc" -o "$work/ctrl/abs.elf" "$work/ctrl/ctrl.o" || fail "control: absolute link failed"
( cd "$work/ctrl" && "$spucc" -o bare.elf ctrl.o ) || fail "control: bare link failed"
DC="$(extract_note "$work/ctrl/abs.elf")" || fail "control: no .note.spu_name in abs.elf"
case "$DC" in
    */ctrl/abs.elf) ;;
    *) fail "control: absolute -o recorded '$DC', want a path ending in /ctrl/abs.elf" ;;
esac
DB="$(extract_note "$work/ctrl/bare.elf")" || fail "control: no .note.spu_name in bare.elf"
[ "$DB" = "bare.elf" ] || fail "control: bare -o recorded '$DB', want 'bare.elf'"
printf '  control: absolute -o records %s; bare -o records %s
' "$DC" "$DB"

echo "spu-name-note-path: PASS"

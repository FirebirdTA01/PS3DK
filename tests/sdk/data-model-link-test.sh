#!/usr/bin/env bash
# A link that mixes ILP32 and LP64 objects is refused (GCC patch 0058,
# binutils patch 0006).  Both data models are ELFCLASS64 with the same
# e_flags; GCC tags every object with Tag_GNU_Power_CellOS_Data_Model
# (GNU attribute 16: 1 ILP32, 2 LP64) and ld refuses tagged inputs that
# disagree.  Before, an -mlp64 link that found an ILP32 library in the flat
# SDK directory linked it silently and crashed at run time.
#
# Rows (one library function lib_get, a main that calls it):
#   tags      cc -c tags ILP32 and -mlp64 objects (the -S output carries
#             the .gnu_attribute line); that output assembled without the
#             line, as an older compiler wrote it, carries no tag.
#   refused   LP64 main + ILP32 archive, and ILP32 main + LP64 archive:
#             the driver exits 1, ld names the archive member and its model,
#             and no ELF is written.
#   links     LP64 + LP64, ILP32 + ILP32, and LP64 main + the untagged
#             archive: rc 0, ELF written; the matched links carry
#             the model's tag.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: data-model-link-test.sh [--ps3dev DIR] [-B DIR]
#   -B DIR is passed to the driver (a directory holding a candidate cc1,
#   as and ld); a powerpc64-ps3-elf-readelf in DIR is used if present.
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
    echo "data-model-link: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
fail() { echo "data-model-link: FAIL: $*"; exit 1; }
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
[ -x "$cc" ] || [ -x "$cc.exe" ] || fail "no compiler at $cc"
readelf="$ps3dev/ppu/bin/powerpc64-ps3-elf-readelf"
ar="$ps3dev/ppu/bin/powerpc64-ps3-elf-ar"
if [ -n "$bdir" ]; then
    for t in readelf ar; do
        for c in "$bdir/powerpc64-ps3-elf-$t" "$bdir/$t"; do
            if [ -x "$c" ] || [ -x "$c.exe" ]; then eval "$t=\$c"; break; fi
        done
    done
fi
B=(); [ -n "$bdir" ] && B=("-B$bdir")
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

cat > lib.c <<'EOF'
struct rec { long n; void *p; };
long lib_get(struct rec *r) { return r->n; }
EOF
cat > main.c <<'EOF'
struct rec { long n; void *p; };
extern long lib_get(struct rec *);
int main(void) { struct rec r = { 3, 0 }; return (int)lib_get(&r) - 3; }
EOF
compile() {  # <out.o> <src> [flags...]
    local out="$1" src="$2"; shift 2
    "$cc" "${B[@]}" "$@" -O1 -c "$src" -o "$out" > "$out.log" 2>&1 \
        || fail "compile $src $*: $(head -3 "$out.log")"
}
compile main32.o main.c
compile main64.o main.c -mlp64
compile lib32.o lib.c
compile lib64.o lib.c -mlp64
# An untagged LP64 object, as an older compiler wrote it: the -S output with
# the data-model line dropped.
"$cc" "${B[@]}" -mlp64 -O1 -S lib.c -o lib64.s > lib64.s.log 2>&1 || fail "cc -S lib.c: $(head -3 lib64.s.log)"
grep -q '^[[:space:]]*\.gnu_attribute 16, 2' lib64.s || fail "lib64.s has no .gnu_attribute 16, 2 line"
grep -v '^[[:space:]]*\.gnu_attribute 16,' lib64.s > libasm.s
compile libasm.o libasm.s -mlp64
for v in 32 64 asm; do
    rm -f "lib$v.a"
    "$ar" rcs "lib$v.a" "lib$v.o" || fail "ar lib$v.a"
done

# ---- tags --------------------------------------------------------------------
tag_of() {  # <file>: ILP32, LP64, or none
    local t
    t="$("$readelf" -A "$1" 2>&1 | sed -n 's/.*Tag_GNU_Power_CellOS_Data_Model: *//p' | tr -d '\r')"
    printf '%s' "${t:-none}"
}
for row in "main32.o ILP32" "lib32.o ILP32" "main64.o LP64" "lib64.o LP64" "libasm.o none"; do
    f="${row%% *}"; want="${row#* }"
    got="$(tag_of "$f")"
    [ "$got" = "$want" ] || fail "$f: data-model tag '$got', expected '$want'"
done
echo "  ok: tags: ILP32 and -mlp64 objects tagged, the object without the line untagged"

# ---- refused -------------------------------------------------------------------
refused() {  # <name> <main.o> <lib> <model the lib uses> [flags...]
    local name="$1" m="$2" lib="$3" model="$4"; shift 4
    local rc=0
    rm -f "$name.elf"
    "$cc" "${B[@]}" "$@" "$m" -L. "-l$lib" -o "$name.elf" > "$name.log" 2>&1 || rc=$?
    [ "$rc" -eq 1 ] || fail "$name: expected exit 1, got $rc ($(head -2 "$name.log"))"
    grep -q "lib$lib\.a(lib$lib\.o) uses the $model data model, .* uses .*; ILP32 and LP64 objects cannot be linked together" "$name.log" \
        || fail "$name: expected ld to name lib$lib.a(lib$lib.o) as $model, got: $(head -3 "$name.log")"
    [ ! -e "$name.elf" ] || fail "$name: an ELF was written"
    echo "  ok: $name refused, lib$lib.a(lib$lib.o) named as $model"
}
refused lp64-main-ilp32-lib main64.o 32 ILP32 -mlp64
refused ilp32-main-lp64-lib main32.o 64 LP64

# ---- links -----------------------------------------------------------------------
links() {  # <name> <main.o> <lib> <model tag of the ELF> [flags...]
    local name="$1" m="$2" lib="$3" want="$4"; shift 4
    local rc=0
    rm -f "$name.elf"
    "$cc" "${B[@]}" "$@" "$m" -L. "-l$lib" -o "$name.elf" > "$name.log" 2>&1 || rc=$?
    [ "$rc" -eq 0 ] || fail "$name: exit $rc ($(head -3 "$name.log"))"
    [ -s "$name.elf" ] || fail "$name: no ELF written"
    local got; got="$(tag_of "$name.elf")"
    [ "$got" = "$want" ] || fail "$name: ELF data-model tag '$got', expected '$want'"
    echo "  ok: $name links, ELF tag $want"
}
links lp64-both main64.o 64 LP64 -mlp64
links ilp32-both main32.o 32 ILP32
links lp64-main-asm-lib main64.o asm LP64 -mlp64

echo "data-model-link: PASS"

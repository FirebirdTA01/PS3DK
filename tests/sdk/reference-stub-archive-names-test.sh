#!/usr/bin/env bash
# A reference Makefile links an import library as -l<name>_stub, so every
# lib<name>_stub.a the reference SDK ships has to exist in our install under
# that exact name, or the link fails before anything else is checked.  Six were
# shipped under names of our own (libcellFont_stub.a for libfont_stub.a, ...)
# and 23 were not installed at all, although nidgen had a database for each.
#
# tools/nidgen/nids/extracted/ holds one database per reference archive, named
# after it, so that directory is the list.  Two checks:
#
#   A. (host only, always runs) scripts/build-cell-stub-archives.sh builds
#      every database under its own name: it is in STUB_YAMLS and its
#      archive_name (or library) gives lib<archive_name>_stub.a == the
#      database's file name, or another listed database produces that name
#      (libfs_stub from cellFs.yaml), or it is one of the two runtime link
#      names the script writes itself.  A database nobody builds is how the
#      23 went missing.
#   B. (needs an install: PS3DK and PS3DEV set) for every database, in both
#      ABIs, $PS3DK/ppu/lib[/lp64]/<name>.a exists and is the right library:
#      a nidgen archive DEFINES __nidgen_<library>_fnid_anchor (a mere
#      reference to it does not count).
#      scripts/build-cell-stub-archives.sh runs this against the tree it has
#      just installed; without an install B skips itself.
#
# libm_stub.a and libstdc++_stub.a are the toolchain's own runtimes rather
# than import libraries (see scripts/build-cell-stub-archives.sh): libm_stub.a
# must be the linker script INPUT(-lm) and libstdc++_stub.a an empty archive.
#
# Each check carries adversarial self-tests and must reject them.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
db_dir="$root/tools/nidgen/nids/extracted"
script="$root/scripts/build-cell-stub-archives.sh"
status=0

fail() { echo "reference-stub-archive-names: FAIL: $*" >&2; status=1; }
note() { echo "reference-stub-archive-names: $*"; }

field() {   # field <yaml> <key>
    sed -n "s/^$2:[[:space:]]*//p" "$1" | head -n 1 | tr -d '\r'
}

# ------------------------------------------------- A. every database is built

# check_script <script> [quiet] - 0 when every database is built under its name.
check_script() {
    local s="$1" quiet="${2:-}" db name listed produced bad=0 y an
    say_bad() { [ -n "$quiet" ] || echo "  $*" >&2; bad=1; }
    # Archive names produced by every YAML the script lists.
    produced=$(grep -oE 'tools/nidgen/nids/[A-Za-z0-9_/+.-]+\.yaml' "$s" | sort -u |
        while read -r y; do
            an=$(field "$root/$y" archive_name)
            [ -n "$an" ] || an=$(field "$root/$y" library)
            echo "lib${an}_stub"
        done)
    for db in "$db_dir"/*.yaml; do
        name=$(basename "$db" .yaml)
        case "$name" in
            libm_stub|libstdc++_stub)
                grep -qF "\"\$install_dir/$name.a\"" "$s" \
                    || say_bad "$name.a is never written by $(basename "$s")"
                continue ;;
        esac
        printf '%s\n' "$produced" | grep -qxF "$name" \
            || say_bad "$name has a database but $(basename "$s") never builds an archive of that name"
    done
    return $bad
}

if check_script "$script"; then
    note "A ok: every database in extracted/ is built under its reference name"
else
    fail "build-cell-stub-archives.sh leaves reference archive names unbuilt (see above)"
fi

selftest=$(mktemp -d)
trap 'rm -rf "$selftest"' EXIT

sed '/extracted\/libsysutil_photo_stub\.yaml/d' "$script" > "$selftest/unlisted.sh"
if check_script "$selftest/unlisted.sh" quiet; then
    fail "A self-test 1: a database missing from STUB_YAMLS was accepted"
else
    note "A self-test 1 ok: an unbuilt database is rejected"
fi

sed '/install_dir\/libm_stub\.a/d' "$script" > "$selftest/nolibm.sh"
if check_script "$selftest/nolibm.sh" quiet; then
    fail "A self-test 2: a script that never writes libm_stub.a was accepted"
else
    note "A self-test 2 ok: a missing runtime link name is rejected"
fi

# ------------------------------------------------ B. the installed archives

if [ -z "${PS3DK:-}" ] || [ -z "${PS3DEV:-}" ] || [ ! -d "${PS3DK}/ppu/lib" ]; then
    note "B skipped: no installed tree (PS3DK/PS3DEV unset); build-cell-stub-archives.sh runs it"
    exit $status
fi
nm="$PS3DEV/ppu/bin/powerpc64-ps3-elf-nm"

# check_lib_dir <dir> [quiet] - 0 when every database has a correct archive.
check_lib_dir() {
    local dir="$1" quiet="${2:-}" db name lib a bad=0
    say_bad() { [ -n "$quiet" ] || echo "  $dir: $*" >&2; bad=1; }
    for db in "$db_dir"/*.yaml; do
        name=$(basename "$db" .yaml)
        a="$dir/$name.a"
        if [ ! -f "$a" ]; then
            say_bad "$name.a missing"
            continue
        fi
        case "$name" in
            libm_stub)
                grep -qx 'INPUT(-lm)' "$a" || say_bad "$name.a is not the INPUT(-lm) linker script"
                continue ;;
            libstdc++_stub)
                [ "$(cat "$a")" = '!<arch>' ] || say_bad "$name.a is not an empty archive"
                continue ;;
        esac
        lib=$(field "$db" library)
        # --defined-only: an archive that merely REFERENCES the anchor (nm
        # lists it as U) carries no stubs of that library.
        "$nm" --defined-only "$a" 2>/dev/null | grep -q " __nidgen_${lib}_fnid_anchor\$" \
            || say_bad "$name.a does not define library $lib"
    done
    return $bad
}

for sub in "" /lp64; do
    if check_lib_dir "$PS3DK/ppu/lib$sub"; then
        note "B ok: all $(ls "$db_dir"/*.yaml | wc -l) reference archive names present in ppu/lib$sub"
    else
        fail "ppu/lib$sub is missing reference archive names (see above)"
    fi
done

# Self-tests on a copy of the ILP32 directory.
mutant="$selftest/lib"
mkdir -p "$mutant"
cp -L "$PS3DK"/ppu/lib/*_stub.a "$mutant/"
# The unmodified copy must pass, or a rejection below proves nothing.
check_lib_dir "$mutant" quiet || fail "B self-test baseline: the unmodified copy is rejected"

rm "$mutant/libsysutil_photo_stub.a"
if check_lib_dir "$mutant" quiet; then
    fail "B self-test 1: a missing libsysutil_photo_stub.a was accepted"
else
    note "B self-test 1 ok: a missing archive is rejected"
fi
cp -L "$PS3DK/ppu/lib/libsysutil_photo_stub.a" "$mutant/"

cp "$mutant/libsysutil_game_exec_stub.a" "$mutant/libsysutil_game_stub.a"
if check_lib_dir "$mutant" quiet; then
    fail "B self-test 2: libsysutil_game_stub.a holding cellGameExec was accepted"
else
    note "B self-test 2 ok: another library under the right name is rejected"
fi
cp -L "$PS3DK/ppu/lib/libsysutil_game_stub.a" "$mutant/"

# 3. an archive that only references the right anchor, and defines nothing.
printf '\t.data\n\t.long __nidgen_cellGame_fnid_anchor\n' \
    | "$PS3DEV/ppu/bin/powerpc64-ps3-elf-as" -o "$selftest/dangling.o" -
rm -f "$mutant/libsysutil_game_stub.a"
"$PS3DEV/ppu/bin/powerpc64-ps3-elf-ar" rc "$mutant/libsysutil_game_stub.a" "$selftest/dangling.o"
"$nm" "$mutant/libsysutil_game_stub.a" | grep -q ' U __nidgen_cellGame_fnid_anchor$' \
    || fail "B self-test 3 did not build a dangling-reference archive; the result below proves nothing"
if check_lib_dir "$mutant" quiet; then
    fail "B self-test 3: an archive that only references the anchor was accepted"
else
    note "B self-test 3 ok: an undefined anchor reference is rejected"
fi

exit $status

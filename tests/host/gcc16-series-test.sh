#!/usr/bin/env bash
# patches/ppu/gcc-16.x is the gcc-12.x series ported to releases/gcc-16.1.0
# (docs/design/gcc-transition.md, stage 2 port map).  This holds the port to
# the map:
#
#  1. the two series list the same patches and hold the same files;
#  2. every patch is byte-identical to its gcc-12.x original except the ones
#     the map names; a newly changed patch, or a named one that is identical
#     again, fails (the map and the tree must move together);
#  3. every patch keeps its original header text;
#  4. context refreshes (0005, 0048, 0061, 0067) add and remove exactly the
#     lines the original does;
#  5. rewrites: 0001 and 0024 differ from the original only in their tm_file
#     lines, which lose dbxelf.h and usegas.h, and no gcc-16.x patch adds a
#     line naming either header; 0009 and 0010 each add the .opt.urls file
#     beside the .opt they create, and otherwise match the original;
#  6. controls: the checks reject a planted copy whose refreshed patch
#     changes one added line, and one whose identical patch gains a header
#     line;
#  8. GCC-16-only patches (gcc16only, e.g. 0068): listed in the gcc-16.x
#     series, absent from gcc-12.x, and left out of checks 1-5; 12.4
#     stays the unchanged control;
#  7. with --gcc-git <gcc repo>: the series applies with --fuzz=0 to a fresh
#     archive of releases/gcc-16.1.0, with no reject or backup files, and,
#     with --expect-tree <dir>, the result equals that tree (ignoring
#     backup and reject files the older tree may hold).
#
# usage: gcc16-series-test.sh [--gcc-git <repo> [--expect-tree <dir>]]
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gcc_git=""; expect_tree=""
while [ $# -gt 0 ]; do
    case $1 in
        --gcc-git) gcc_git=$2; shift 2 ;;
        --expect-tree) expect_tree=$2; shift 2 ;;
        *) echo "usage: $0 [--gcc-git <repo> [--expect-tree <dir>]]"; exit 2 ;;
    esac
done
status=0
fail() { echo "gcc16-series: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

refreshed="0005 0048 0061 0067"
tmfile="0001 0024"
opturls="0009 0010"
named="$refreshed $tmfile $opturls"
# Patches with no gcc-12.x original (check 8).
gcc16only="0068 0072 0073"
only16() { local n=${1%%-*}; case " $gcc16only " in *" $n "*) return 0 ;; esac; return 1; }
ported() { while read -r l; do only16 "$l" || echo "$l"; done; }

header() { awk '/^(diff --git |--- [^ ]|Index: )/{exit} {print}' "$1"; }
# The added and removed lines of a patch, sorted, without the file headers.
changes() { grep -E '^[+-]' "$1" | grep -vE '^(\+\+\+|---) ' | sort; }

# check <old dir> <new dir>: print one line per problem.
check() {
    local old=$1 new=$2 p n id
    cmp -s "$old/series" <(ported < "$new/series") || echo "series lists differ"
    diff <(cd "$old" && ls) <(cd "$new" && ls | ported) > /dev/null || echo "file sets differ"
    for n in $gcc16only; do
        ls "$new"/$n-*.patch > /dev/null 2>&1 || echo "GCC-16-only $n missing from gcc-16.x"
        grep -q "^$n-" "$new/series" || echo "GCC-16-only $n not in the gcc-16.x series"
        ls "$old"/$n-*.patch > /dev/null 2>&1 && echo "GCC-16-only $n also exists in gcc-12.x"
    done
    for p in $(grep -v '^#' "$new/series" | grep -v '^[[:space:]]*$'); do
        n=${p%%-*}
        only16 "$p" && continue
        [ -f "$old/$p" ] && [ -f "$new/$p" ] || { echo "$p missing"; continue; }
        case " $named " in *" $n "*) id=named ;; *) id=same ;; esac
        if cmp -s "$old/$p" "$new/$p"; then
            [ $id = named ] && echo "$p is identical to gcc-12.x but the port map names it"
            continue
        fi
        [ $id = same ] && { echo "$p changed but the port map does not name it"; continue; }
        cmp -s <(header "$old/$p") <(header "$new/$p") || echo "$p header text changed"
        case " $refreshed " in *" $n "*)
            cmp -s <(changes "$old/$p") <(changes "$new/$p") || echo "$p refresh changed added/removed lines" ;;
        esac
        case " $tmfile " in *" $n "*)
            local d
            d=$(diff <(changes "$old/$p" | grep -v 'tm_file=') <(changes "$new/$p" | grep -v 'tm_file='))
            [ -z "$d" ] || echo "$p differs beyond its tm_file lines"
            changes "$old/$p" | grep 'tm_file=' | sed 's/ dbxelf\.h / /; s/ usegas\.h / /' > "$work/want"
            changes "$new/$p" | grep 'tm_file=' > "$work/got"
            cmp -s "$work/want" "$work/got" || echo "$p tm_file lines are not the originals minus dbxelf.h and usegas.h" ;;
        esac
        case " $opturls " in *" $n "*)
            local opt
            opt=$(grep -oE '^\+\+\+ b/gcc/config/rs6000/[a-z0-9-]+\.opt$' "$new/$p" | sed 's|^+++ b/||')
            [ -n "$opt" ] || echo "$p creates no .opt"
            grep -qx "+++ b/$opt.urls" "$new/$p" || echo "$p adds no $opt.urls"
            # Without the .opt.urls hunk, the added/removed lines are the original's.
            awk -v u="b/$opt.urls" '/^diff --git /{skip = ($4 == u)} !skip' "$new/$p" > "$work/nourls"
            cmp -s <(changes "$old/$p") <(changes "$work/nourls") || echo "$p differs beyond its .opt.urls file" ;;
        esac
    done
    grep -hE '^\+' "$new"/*.patch | grep -vE '^\+\+\+ ' | grep -E 'dbxelf\.h|usegas\.h' | sed 's/^/adds a line naming a removed header: /'
}

old="$root/patches/ppu/gcc-12.x"; new="$root/patches/ppu/gcc-16.x"
check "$old" "$new" > "$work/problems"
if [ -s "$work/problems" ]; then
    fail "the port does not match its map:"; sed 's/^/    /' "$work/problems"
else
    echo "gcc16-series: ok   $(grep -vc '^#\|^[[:space:]]*$' "$new/series") patches: $(echo $named | wc -w) named by the port map, $(echo $gcc16only | wc -w) GCC-16-only, the rest byte-identical"
fi

# Controls: the checks must catch a changed line in a refresh and a stray edit
# in an identical patch.
for c in refresh same; do
    rm -rf "$work/c"; cp -r "$new" "$work/c"
    if [ $c = refresh ]; then
        f=$(ls "$work/c"/0067-*.patch)
        awk '!done && /^\+[^+]/ {print $0 " "; done=1; next} {print}' "$f" > "$f.t" && mv "$f.t" "$f"
        want="refresh changed added/removed lines"
    else
        f=$(ls "$work/c"/0002-*.patch)
        { echo "planted header line"; cat "$f"; } > "$f.t" && mv "$f.t" "$f"
        want="changed but the port map does not name it"
    fi
    if check "$old" "$work/c" | grep -q "$want"; then
        echo "gcc16-series: ok   control: a planted $c mutant is reported"
    else
        fail "control: the planted $c mutant was not reported"
    fi
done

if [ -n "$gcc_git" ]; then
    mkdir -p "$work/apply"
    if ! git -C "$gcc_git" archive --prefix=src/ releases/gcc-16.1.0 | tar -x -C "$work/apply"; then
        fail "cannot archive releases/gcc-16.1.0 from $gcc_git"
    else
        applied=0
        for p in $(grep -v '^#' "$new/series" | grep -v '^[[:space:]]*$'); do
            if (cd "$work/apply/src" && patch -p1 --forward --fuzz=0 --no-backup-if-mismatch -s) < "$new/$p" > "$work/apply.out" 2>&1; then
                applied=$((applied + 1))
            else
                fail "$p does not apply with --fuzz=0"; sed 's/^/    /' "$work/apply.out"; break
            fi
        done
        left=$(find "$work/apply/src" \( -name '*.rej' -o -name '*.orig' \) | wc -l)
        [ "$left" -eq 0 ] || fail "$left reject/backup files after applying"
        echo "gcc16-series: ok   $applied patches applied to releases/gcc-16.1.0 with --fuzz=0"
        if [ -n "$expect_tree" ]; then
            # The fresh tree has no backups (checked above); an older tree may.
            if diff -r -q -x "*.orig" -x "*.rej" "$expect_tree" "$work/apply/src" > "$work/tree.diff"; then
                echo "gcc16-series: ok   the patched tree equals $expect_tree"
            else
                fail "the patched tree differs from $expect_tree:"; head -20 "$work/tree.diff" | sed 's/^/    /'
            fi
        fi
    fi
fi

[ $status -eq 0 ] && echo "PASS: gcc16-series-test"
exit $status

#!/usr/bin/env bash
# gen-make-sprx-source.sh <make_self.c> <output.c>
#
# Emits the make_sprx source: a build-tree copy of PSL1GHT's geohot
# make_self.c with the SPRX container relabel applied.  Both the Windows
# cross-build (scripts/build-host-tools-windows.sh) and the native tools
# step (scripts/build-psl1ght.sh) compile the emitted file with -DSPRX;
# this script is the ONLY place the relabel and its drift guards live.
#
# Why the relabel: the SPRX branch labels the container se_flags=7 while
# the KEY() macro — which selects on NPDRM only, never on SPRX — encrypts
# it with the same appold material the plain .self uses.  The loader looks
# a key up by (program_type, se_flags, sceversion), so a container that
# says "revision 7" and is encrypted with the revision-1 key cannot be
# decrypted: RPCS3 fails it with CELL_PRX_ERROR_UNSUPPORTED_PRX_TYPE,
# "Failed to decrypt file", before the ELF is ever parsed.  Verified by
# flipping the two bytes in a built .sprx, after which the same file
# decrypts.
#
# The vendored source is never modified; the substitution is asserted in
# both directions so this fails loudly if upstream ever changes the line.

set -euo pipefail

die() { printf "[gen-make-sprx-src] ERROR: %s\n" "$*" >&2; exit 1; }

[[ $# -eq 2 ]] || die "usage: gen-make-sprx-source.sh <make_self.c> <output.c>"
src="$1"
dst="$2"

[[ -f "$src" ]] || die "make_self.c not found: $src"
mkdir -p "$(dirname "$dst")"

# Guard on the SOURCE, by count.  A bare "the 1-form exists in the output"
# check is satisfiable without the relabel ever applying: make_self.c's
# plain-SELF branch already contains its own se_flags=1 line, so the only
# reliable signal is the exact number of 7-form sites going 1 -> 0.
n7=$(grep -c 'set_u16(&(hdr->s_flags), 7);' "$src" || true)
[[ "$n7" -eq 1 ]] \
    || die "expected exactly one se_flags=7 site in $src, found $n7 — upstream make_self.c changed; re-verify the relabel before trusting make_sprx output"

sed 's/set_u16(&(hdr->s_flags), 7);/set_u16(\&(hdr->s_flags), 1);/' \
    "$src" > "$dst"
if grep -q 'set_u16(&(hdr->s_flags), 7);' "$dst"; then
    die "se_flags 7 still present after the relabel"
fi

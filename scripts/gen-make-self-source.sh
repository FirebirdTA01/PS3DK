#!/usr/bin/env bash
# gen-make-self-source.sh <make_self.c> <output.c>
#
# Emits a build-tree copy of PSL1GHT's geohot make_self.c whose certified-file header
# data length (the u64 at +0x18) is the size of the data the file actually
# stores after the header: file size minus header length.
#
# Upstream sets it to the input ELF's size.  The segments are stored
# deflated, so the header then claims more data than the file holds.  A
# loader that sanity-checks the header against the file refuses such a file
# before loading it: PS3HEN's VSH plugin loader returns EINVAL when the file
# is shorter than header length + data length, so every real-signed .sprx
# from make_sprx was refused as a VSH plugin while it loaded fine in a game
# process.  Firmware-signed files and scetool's writer both carry the stored
# size.
#
# make_self, make_self_npdrm and make_sprx are one program, so every build of
# them compiles this output (make_sprx through gen-make-sprx-source.sh).  The
# vendored source is never modified; both anchors are asserted so this fails
# loudly if upstream changes either line.
set -euo pipefail
die() { printf "[gen-make-self-src] ERROR: %s\n" "$*" >&2; exit 1; }
[[ $# -eq 2 ]] || die "usage: gen-make-self-source.sh <make_self.c> <output.c>"
src="$1"
dst="$2"
[[ -f "$src" ]] || die "make_self.c not found: $src"
mkdir -p "$(dirname "$dst")"
# Work on an LF copy: a checkout made with CRLF line endings would defeat the
# line anchors below (the generated file is a build input only, never committed).
lf="$dst.lf"
tr -d '\r' < "$src" > "$lf"
trap 'rm -f "$lf"' EXIT
src_name="$src"
src="$lf"

# The upstream assignment this overrides, and the point where the file is
# complete (running_size is then the file size) and not yet written to memory
# or signed: the override must land before the header is copied and hashed.
n_set=$(grep -c 'set_u64(&output_self_header.s_exsize, input_elf_len);' "$src" || true)
[[ "$n_set" -eq 1 ]] \
    || die "expected exactly one s_exsize = input_elf_len site in $src_name, found $n_set: upstream make_self.c changed; re-verify the data length fix"
n_done=$(grep -c '^  // \*\*\*DONE\*\*\*$' "$src" || true)
[[ "$n_done" -eq 1 ]] \
    || die "expected exactly one '  // ***DONE***' line in $src_name, found $n_done: upstream make_self.c changed; re-verify where the file is complete"
n_write=$(grep -c '^  write_self_file_in_memory();$' "$src" || true)
[[ "$n_write" -eq 1 ]] \
    || die "expected exactly one write_self_file_in_memory() call in $src_name, found $n_write"
done_line=$(grep -n '^  // \*\*\*DONE\*\*\*$' "$src" | cut -d: -f1)
write_line=$(grep -n '^  write_self_file_in_memory();$' "$src" | cut -d: -f1)
[[ "$done_line" -lt "$write_line" ]] \
    || die "'// ***DONE***' (line $done_line) is not before write_self_file_in_memory() (line $write_line)"

fix='  set_u64(\&output_self_header.s_exsize, running_size - get_u64(\&(output_self_header.s_shsize))); /* PS3DK: stored data length, not the input ELF size */'
sed "/^  \/\/ \*\*\*DONE\*\*\*\$/a\\
$fix" "$src" > "$dst"

n_fix=$(grep -c 'PS3DK: stored data length' "$dst" || true)
[[ "$n_fix" -eq 1 ]] || die "data length fix landed $n_fix times in $dst, expected once"

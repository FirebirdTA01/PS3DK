#!/usr/bin/env bash
# PS3 Custom Toolchain — PSL1GHT-derived host tools for the Linux tools tarball.
#
# The Windows zip has always shipped bin2s, cgcomp, fself, make_self,
# make_self_npdrm, make_sprx and package_finalize (build-host-tools-windows.sh),
# plus fself.py, Struct.py, sfo.xml and ICON0.PNG.  The Linux tools tarball
# shipped none of them, so a Linux user with only the release packages could
# not turn an .elf into a .self or a .pkg.  This builds the same programs from
# the same sources with the host compiler and stages them, with the same data
# files, into OUT_DIR (the tarball's bin/).
#
# Link flags mirror the Windows build: gmp + libcrypto + zlib for the geohot
# tools, zlib for fself.  Host packages: libgmp-dev libssl-dev zlib1g-dev.
#
# Usage: scripts/build-psl1ght-host-tools-linux.sh OUT_DIR

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd "$script_dir/.." && pwd -P)"

say() { printf "[psl1ght-host-tools] %s\n" "$*"; }
die() { printf "[psl1ght-host-tools] ERROR: %s\n" "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "usage: $0 OUT_DIR"
out="$1"
mkdir -p "$out"

# The PSL1GHT commit bootstrap.sh pins, so these tools come from the same
# source as the PSL1GHT runtime the SDK is built from.
pin="$(sed -n 's/^PSL1GHT_COMMIT="\([0-9a-f]*\)".*/\1/p' "$script_dir/bootstrap.sh")"
[[ ${#pin} -eq 40 ]] || die "could not read PSL1GHT_COMMIT from scripts/bootstrap.sh"
src="$root/src/ps3dev/PSL1GHT"
# Depth 1 at the pin: nothing reads the history.
if [[ ! -d "$src/.git" ]]; then
    say "fetching PSL1GHT $pin"
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" remote add origin https://github.com/ps3dev/PSL1GHT.git
fi
git -C "$src" checkout -q "$pin" 2>/dev/null \
    || { git -C "$src" fetch -q --depth 1 origin "$pin" && git -C "$src" checkout -q "$pin"; } \
    || die "cannot check out PSL1GHT $pin"
say "PSL1GHT at $(git -C "$src" rev-parse HEAD)"

t="$src/tools"
for f in generic/bin2s.c geohot/make_self.c geohot/package_finalize.c \
         ps3py/fself.py ps3py/Struct.py; do
    [[ -f "$t/$f" ]] || die "PSL1GHT source missing: tools/$f"
done
[[ -d "$t/fself/source" && -d "$t/cgcomp/source" ]] || die "PSL1GHT fself/cgcomp sources missing"

# Same reproducibility flags as the Windows build.
repro="-ffile-prefix-map=$root=. -ffile-prefix-map=$src=psl1ght"
IFS=' ' read -r -a cc <<< "${CC:-cc}"
IFS=' ' read -r -a cxx <<< "${CXX:-c++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"${cc[@]}" -O2 -Wall $repro "$t/generic/bin2s.c" -o "$out/bin2s"

crypto=(-lgmp -lcrypto -lz)
"${cc[@]}" -O2 $repro "$t/geohot/make_self.c" "${crypto[@]}" -o "$out/make_self"
"${cc[@]}" -O2 $repro -DNPDRM "$t/geohot/make_self.c" "${crypto[@]}" -o "$out/make_self_npdrm"
"$script_dir/gen-make-sprx-source.sh" "$t/geohot/make_self.c" "$work/make_self_sprx.c"
"${cc[@]}" -O2 $repro -I"$t/geohot" -DSPRX "$work/make_self_sprx.c" "${crypto[@]}" -o "$out/make_sprx"
"${cc[@]}" -O2 $repro "$t/geohot/package_finalize.c" "${crypto[@]}" -o "$out/package_finalize"

mapfile -t fself_srcs < <(find "$t/fself/source" -maxdepth 1 -name '*.c' -print | sort)
[[ ${#fself_srcs[@]} -gt 0 ]] || die "no .c sources under tools/fself/source"
"${cc[@]}" -O2 $repro -I"$t/fself/include" "${fself_srcs[@]}" -lz -o "$out/fself"

mapfile -t cgcomp_srcs < <(find "$t/cgcomp/source" -maxdepth 1 -name '*.cpp' -print | sort)
[[ ${#cgcomp_srcs[@]} -gt 0 ]] || die "no .cpp sources under tools/cgcomp/source"
"${cxx[@]}" -std=c++11 -O2 $repro -I"$t/cgcomp/include" "${cgcomp_srcs[@]}" -ldl -o "$out/cgcomp"

install -m 0644 "$t/ps3py/fself.py" "$out/fself.py"
install -m 0644 "$t/ps3py/Struct.py" "$out/Struct.py"
chmod +x "$out/fself.py"
install -m 0644 "$root/tools/sfo-pkg/sfo.xml" "$out/sfo.xml"
install -m 0644 "$root/sdk/assets/ICON0.PNG" "$out/ICON0.PNG"

say "staged bin2s cgcomp fself make_self make_self_npdrm make_sprx package_finalize fself.py Struct.py sfo.xml ICON0.PNG"

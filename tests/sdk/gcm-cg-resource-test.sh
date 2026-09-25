#!/usr/bin/env bash
# Host runtime/type checks always run. --ps3dev adds serial C/C++ PPU probes
# for both ABIs; it never installs into or modifies the supplied prefix.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
ps3dev=
out=
while (($#)); do
    case $1 in
        --ps3dev) ps3dev=$2; shift 2 ;;
        --output) out=$2; shift 2 ;;
        *) echo "usage: $0 [--ps3dev PREFIX] [--output NEW_DIRECTORY]" >&2; exit 2 ;;
    esac
done
if [[ -n $out ]]; then mkdir -- "$out"; else out=$(mktemp -d); fi
out=$(cd "$out" && pwd)
exec > >(tee "$out/run.log") 2>&1
echo "gcm-cg-resource: output=$out"
printf 'script=%q\nroot=%q\nps3dev=%q\n' "$0" "$root" "$ps3dev"
date -u +'%Y-%m-%dT%H:%M:%SZ'
sha256sum "$0" "$root/tests/sdk/gcm-cg-resource-test.c" \
    "$root/sdk/libgcm_cmd/src/gcm_cg.c" \
    "$root/sdk/libgcm_cmd/include/cell/gcm/gcm_cg_func.h" > "$out/inputs.sha256"
# Record the actual canonical Cg header family, including macro data files.
find "$root/sdk/include/Cg" -type f -print0 | sort -z | xargs -0 sha256sum >> "$out/inputs.sha256"
flags=(-O0 -Wall -Wextra -Werror -I"$root/sdk/include" -I"$root/sdk/libgcm_cmd/include")
source="$root/sdk/libgcm_cmd/src/gcm_cg.c"
consumer="$root/tests/sdk/gcm-cg-resource-test.c"
status=0
run() { printf '+'; printf ' %q' "$@"; printf '\n'; "$@"; }
record_tool() {
    local tool=$1
    command -v "$tool"
    sha256sum "$(command -v "$tool")"
    "$tool" --version | head -n 1
    local internal
    for internal in cc1 cc1plus; do
        local path
        path=$("$tool" -print-prog-name="$internal")
        if [[ -f $path ]]; then sha256sum "$path"; fi
    done
}
check_case() {
    local name=$1 cc=$2 cxx=$3 nm=$4 objdump=$5 target=$6
    shift 6
    local abi=("$@")
    local dir="$out/$name"
    mkdir "$dir"
    record_tool "$cc"
    record_tool "$cxx"
    if ! run "$cc" "${abi[@]}" "${flags[@]}" -std=c11 -MD -MF "$dir/implementation.d" \
        -c "$source" -o "$dir/implementation.o"; then status=1; return; fi
    "$nm" --defined-only "$dir/implementation.o" > "$dir/implementation.nm"
    if ! grep -Eq '[[:space:]]cellGcmCgGetParameterResource$' "$dir/implementation.nm"; then
        echo "FAIL $name: missing C symbol"; status=1; return
    fi
    "$objdump" -dr "$dir/implementation.o" > "$dir/implementation.disassembly"
    for language in c c++; do
        local compiler=$cc standard=c11
        if [[ $language == c++ ]]; then compiler=$cxx; standard=c++17; fi
        local prefix="$dir/$language"
        if ! run "$compiler" "${abi[@]}" "${flags[@]}" -x "$language" -std="$standard" \
            -MD -MF "$prefix.d" -c "$consumer" -o "$prefix.o"; then
            echo "FAIL $name $language: consumer compile"; status=1; continue
        fi
        "$objdump" -r "$prefix.o" > "$prefix.relocations"
        if ! grep -Eq '[[:space:]]\.?cellGcmCgGetParameterResource([+-].*)?$' "$prefix.relocations"; then
            echo "FAIL $name $language: missing C caller relocation"; status=1; continue
        fi
        if [[ $target == host ]]; then
            if ! run "$compiler" "$prefix.o" "$dir/implementation.o" -o "$prefix.exe" || ! run "$prefix.exe"; then
                echo "FAIL $name $language: runtime"; status=1; continue
            fi
        else
            # The compiler driver's default executable script requires a TOC;
            # use its own linker directly for this relocatable-only ABI check.
            if ! run "$("$cc" -print-prog-name=ld)" -r "$prefix.o" "$dir/implementation.o" -o "$prefix.linked.o"; then
                echo "FAIL $name $language: relocatable link"; status=1; continue
            fi
            "$nm" -u "$prefix.linked.o" > "$prefix.undefined"
            if grep -q 'cellGcmCgGetParameterResource' "$prefix.undefined"; then
                echo "FAIL $name $language: unresolved resource query"; status=1; continue
            fi
        fi
        echo "PASS $name $language"
    done
}
check_case host "${HOST_CC:-cc}" "${HOST_CXX:-c++}" nm objdump host
if [[ -n $ps3dev ]]; then
    tool="$ps3dev/ppu/bin/powerpc64-ps3-elf"
    check_case ppu-ilp32 "$tool-gcc" "$tool-g++" "$tool-nm" "$tool-objdump" ppu -DEXPECT_POINTER_SIZE=4
    check_case ppu-lp64 "$tool-gcc" "$tool-g++" "$tool-nm" "$tool-objdump" ppu -mlp64 -DEXPECT_POINTER_SIZE=8
else
    echo 'SKIP PPU probes: supply --ps3dev PREFIX'
fi
python3 - "$out" <<'PY'
import hashlib, pathlib, shlex, sys
out = pathlib.Path(sys.argv[1])
paths = set()
for depfile in out.glob('*/*.d'):
    dependencies = depfile.read_text().replace('\\\n', ' ').split(':', 1)[1]
    paths.update(shlex.split(dependencies))
with (out / 'dependencies.sha256').open('x') as evidence:
    for name in sorted(paths):
        path = pathlib.Path(name)
        evidence.write(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {name}\n')
PY
echo "gcm-cg-resource: status=$status"
exit "$status"

#!/usr/bin/env bash
# Exercise the real release gate. Synthetic archives need only member names;
# target ELF/link/runtime behavior is covered by tests/sdk/ps3dk-prof-auto.
# Removing a manifest row or accepting an incomplete/wrong archive must fail.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
passed=0 failed=0
ar_tool=$(command -v ar)
mkdir -p "$work/tree/ppu/lib/lp64" "$work/objects"
for member in prof_core prof_drain prof_version pal_lv2 prof_arcs_writer startup live_agent live_os_lv2 live_api \
              ext_runtime psp_ppu psp_elf gpu_runtime prg gpu_port_gcm rsxcap prof_identity extra; do
    printf 'membership fixture: %s\n' "$member" > "$work/objects/$member.o"
done
record() {
    if [[ $1 == pass ]]; then
        passed=$((passed+1)); echo "PASS $2"
    else
        failed=$((failed+1)); echo "FAIL $2"; printf '%s\n' "$out"
    fi
}
gate() {
    # Other release sections deliberately reject this synthetic, partial SDK.
    # Inspect this section's actual output; do not claim the whole tree passed.
    local all
    all=$(PS3_AR="$ar_tool" bash "$root/scripts/check-release-tree.sh" "$work/tree" 2>&1 || :)
    out=$(sed -n '/^-- profiler archive membership --$/,/^-- crtend.o /p' <<< "$all")
}
expect() {
    local label=$1 rel=$2 verdict=$3
    gate
    if grep -qF "  $verdict    $rel " <<< "$out"; then
        record pass "$label"
    else
        record fail "$label"
    fi
}

# Each required path is checked through the production presence validator,
# independently of the membership gate. A missing manifest row cannot pass.
all=$(bash "$root/scripts/check-release-tree.sh" "$work/tree" 2>&1 || :)
out=$all
for rel in ppu/include/ps3dk/prof.h ppu/lib/libps3dkprof.a ppu/lib/lp64/libps3dkprof.a \
           ppu/lib/libps3prof.a ppu/lib/lp64/libps3prof.a \
           ppu/include/ps3dk/prof_spu.h ppu/include/ps3dk/psp_desc.h ppu/include/ps3dk/prof_gpu.h \
           ppu/lib/libps3dkprof_ext.a ppu/lib/lp64/libps3dkprof_ext.a \
           ppu/lib/libps3dkprof_ext_live.a ppu/lib/lp64/libps3dkprof_ext_live.a \
           spu/include/ps3dk/prof_spu_lane.h spu/include/ps3dk/psp_desc.h spu/include/ps3dk/psp_spu.h \
           spu/include/ps3dk/psp_layout.h; do
    if grep -qF "missing sdk_core: $rel" <<< "$all"; then
        record pass "required $rel"
    else
        record fail "required $rel"
    fi
done

for abi in '' lp64/; do
    for lib in ps3dkprof ps3prof ps3dkprof_ext ps3dkprof_ext_live; do
        rel="ppu/lib/${abi}lib${lib}.a"
        archive="$work/tree/$rel"
        # An independent copy of the release contract, not read from the gate.
        case $lib in
            ps3dkprof) members=(prof_core.o prof_drain.o prof_version.o pal_lv2.o live_agent.o live_os_lv2.o live_api.o) ;;
            ps3prof) members=(prof_core.o prof_drain.o prof_version.o pal_lv2.o prof_arcs_writer.o startup.o) ;;
            ps3dkprof_ext) members=(prof_core.o prof_drain.o prof_version.o pal_lv2.o ext_runtime.o psp_ppu.o psp_elf.o
                                    gpu_runtime.o prg.o gpu_port_gcm.o rsxcap.o prof_identity.o) ;;
            ps3dkprof_ext_live) members=(live_agent.o live_os_lv2.o live_api.o) ;;
        esac
        restore() {
            rm -f "$archive"
            (cd "$work/objects"; "$ar_tool" rcs "$archive" "${members[@]}")
        }
        restore
        # The spacing matches ok()/bad() in the production release checker.
        expect "valid $rel" "$rel" 'ok  '
        for member in "${members[@]}"; do
            restore
            "$ar_tool" d "$archive" "$member"
            expect "missing $member in $rel" "$rel" FAIL
        done
        restore
        "$ar_tool" r "$archive" "$work/objects/extra.o"
        expect "extra member in $rel" "$rel" FAIL
        restore
        "$ar_tool" q "$archive" "$work/objects/prof_core.o"
        expect "duplicate member in $rel" "$rel" FAIL
        printf 'not an archive\n' > "$archive"
        expect "corrupt $rel" "$rel" FAIL
        rm -f "$archive"
        expect "absent $rel" "$rel" FAIL
        restore
    done
done
# MinGW ar emits CRLF. Preserve the real tool's list and failure status, but
# model its text-mode line endings so Linux CI catches a Windows false reject.
printf '#!/usr/bin/env bash\nset -o pipefail\n%q "$@" | sed '\''s/$/\\r/'\''\n' "$ar_tool" > "$work/crlf-ar"
chmod +x "$work/crlf-ar"
ar_tool="$work/crlf-ar"
gate
if [[ $(grep -c '^  ok      ppu/lib/.*libps3.*\.a has exactly ' <<< "$out") == 8 ]]; then
    record pass 'CRLF ar listing accepts all eight archives'
else
    record fail 'CRLF ar listing accepts all eight archives'
fi
ar_tool=/bin/false
gate
if [[ $(grep -c '^  FAIL    ppu/lib/.*libps3.*\.a ' <<< "$out") == 8 ]]; then
    record pass 'ar failure rejects all eight archives'
else
    record fail 'ar failure rejects all eight archives'
fi
echo "release-tree-profiler: tests=$((passed+failed)) pass=$passed fail=$failed"
[[ $failed == 0 ]]

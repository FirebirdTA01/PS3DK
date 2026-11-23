#!/usr/bin/env bash
# CellOS Lv-2 ABI conformance — implements docs/abi/cellos-lv2-abi-spec.md
# section 7, the parts that run without an emulator:
#
#   7.1  build samples/toolchain/hello-ppu-abi-check with zero warnings and
#        have `abi-verify check` emit all PASS lines;
#   7.2  diff the sample's emitted manifest against
#        tests/abi/fixtures/crt1.json on every common field (ELF identity,
#        .opd entry shape, .sys_proc_prx_param size/magic/version).
#
#   7.3 (run the sample in RPCS3) is NOT done here — it needs an emulator —
#       and is left to a human or the hardware pass.
#
# What it needs to run:
#   * a native PPU cross toolchain ($PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc)
#     AND the SDK headers/libs ($PS3DK) for section 7.1 to build the sample;
#   * the abi-verify binary (from $PS3DEV/bin, or built from tools/abi-verify)
#     for both sections.
#
# If the sample cannot be built (no toolchain), pass --elf so the conformance
# checks run against an ELF that was already produced.
#
# The manifest diff (7.2) is deliberately scoped to fields that are *common*
# to a relocatable fixture (crt1.o) and a final linked executable: ELF class /
# endianness / machine, the .opd descriptor shape (entry stride + head/tail
# reloc *types*, not symbols or offsets), and .sys_proc_prx_param size / magic /
# version.  elf_type (RELOC vs EXEC) and the OS/ABI byte are type-dependent by
# nature — a startfile object is untagged (os_abi 0x00, allowed by abi-verify
# for .o's) while a linked ELF carries 0x66 — so those are reported as info,
# not used to fail the diff.  A field present in one manifest but absent in the
# other is reported as a mismatch (that is exactly the "real ABI finding" a
# section-7 drift should surface), not silently skipped.
#
# usage:
#   abi-conformance.sh [--ps3dev DIR] [--sdk DIR] [--abi-verify PATH]
#                      [--elf PATH] [--sample DIR] [--fixture PATH]
#                      [--build-dir DIR] [--no-build]
#
#   --elf PATH       run the conformance checks against this already-built ELF
#                    and skip the cmake build (implies --no-build for 7.1 build).
#   --no-build       do not build the sample; --elf is then required.
#   --abi-verify P   use this abi-verify binary (default: $PS3DEV/bin then a
#                    cargo build of tools/abi-verify).
set -u

ps3dev="${PS3DEV:-}"
sdk="${PS3DK:-}"
abi_verify=""
elf=""
sample=""
fixture=""
build_dir=""
build=1

while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev)     ps3dev="${2:?missing dir}"; shift 2 ;;
        --sdk)        sdk="${2:?missing dir}"; shift 2 ;;
        --abi-verify) abi_verify="${2:?missing path}"; shift 2 ;;
        --elf)        elf="${2:?missing elf}"; shift 2 ;;
        --sample)     sample="${2:?missing dir}"; shift 2 ;;
        --fixture)    fixture="${2:?missing path}"; shift 2 ;;
        --build-dir)  build_dir="${2:?missing dir}"; shift 2 ;;
        --no-build)   build=0; shift ;;
        -h|--help)    sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "usage: $0 --help" >&2; exit 2 ;;
    esac
done

command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 2; }
[ -n "$ps3dev" ] || { echo "::error::PS3DEV not set (no native PPU toolchain prefix)" >&2; exit 2; }

sample="${sample:-$PWD/samples/toolchain/hello-ppu-abi-check}"
fixture="${fixture:-$PWD/tests/abi/fixtures/crt1.json}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail=0

step() { printf '\n== %s ==\n' "$*"; }
ok()   { printf '  [ok]   %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; fail=1; }

# ------------------------------------------------------------------ abi-verify
if [ -z "$abi_verify" ]; then
    for c in "$ps3dev/bin/abi-verify" "${PS3DK:-}/bin/abi-verify" "abi-verify"; do
        if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then abi_verify="$c"; break; fi
    done
fi
if [ -z "$abi_verify" ]; then
    if command -v cargo >/dev/null 2>&1; then
        step "building abi-verify from tools/"
        cargo build --release --manifest-path "$PWD/tools/Cargo.toml" -p abi-verify \
            >/dev/null 2>&1 || cargo build --release --workspace --manifest-path "$PWD/tools/Cargo.toml" \
            >/dev/null 2>&1 || { echo "could not build abi-verify" >&2; exit 2; }
        abi_verify="$PWD/tools/target/release/abi-verify"
    else
        echo "::error::no abi-verify found and cargo unavailable to build it" >&2; exit 2
    fi
fi
step "abi-verify: $abi_verify"
"$abi_verify" --version

# ------------------------------------------------------------- locate the ELF
if [ -z "$elf" ]; then
    if [ "$build" -ne 1 ]; then
        echo "::error::--no-build given but no --elf to inspect" >&2; exit 2
    fi
    if [ ! -x "$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        echo "::error::no native PPU gcc at $ps3dev/ppu/bin/powerpc64-ps3-elf-gcc" >&2
        echo "          build the native toolchain first, or pass --elf <elf>" >&2
        exit 2
    fi
    command -v cmake >/dev/null 2>&1 || { echo "cmake is required to build the sample" >&2; exit 2; }
    build_dir="${build_dir:-$work/build}"
    tcfile="$PWD/cmake/ps3-ppu-toolchain.cmake"
    step "building $sample"
    if ! cmake -S "$sample" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE="$tcfile" \
            >"$work/cmake.log" 2>&1; then
        cat "$work/cmake.log" >&2; bad "sample cmake configure"; exit 1
    fi
    if ! cmake --build "$build_dir" --verbose >"$work/build.log" 2>&1; then
        cat "$work/build.log" >&2; bad "sample build"; exit 1
    fi
    # 7.1 — zero warnings.  A `warning:` line from the sample's own source is
    # the drift; collect them all so a human sees the exact offender(s).
    if grep -E 'warning:' "$work/build.log" >/dev/null 2>&1; then
        bad "sample built with warnings (section 7.1 requires zero):"
        grep -E 'warning:' "$work/build.log" | sed 's/^/           /'
    else
        ok "sample built with zero warnings"
    fi
    elf="$(find "$build_dir" -maxdepth 1 -type f -name 'hello-ppu-abi-check' 2>/dev/null | head -n1)"
    # fall back to the cmake target name if the sample dir name differs
    [ -n "$elf" ] || elf="$(find "$build_dir" -maxdepth 2 -type f -name '*.elf' 2>/dev/null | head -n1)"
    [ -n "$elf" ] && [ -f "$elf" ] || { bad "could not locate the built sample ELF"; exit 1; }
    echo "  sample ELF: $elf"
else
    [ -f "$elf" ] || { echo "::error::--elf $elf not found" >&2; exit 2; }
    ok "using provided ELF: $elf"
fi

# ------------------------------------------------------------ 7.1 abi-verify check
step "section 7.1 — abi-verify check $elf"
if "$abi_verify" check "$elf" >"$work/check.txt" 2>&1; then
    sed 's/^/  /' "$work/check.txt"
    ok "abi-verify check: all invariants PASS"
else
    sed 's/^/  /' "$work/check.txt"
    bad "abi-verify check: invariants FAILED (see above)"
fi

# ------------------------------------------------------- 7.2 manifest vs fixture
step "section 7.2 — manifest diff vs $fixture"
"$abi_verify" manifest "$elf" >"$work/sample.json" 2>"$work/man.err"
rc_man=$?
if [ "$rc_man" -ne 0 ]; then
    sed 's/^/  /' "$work/man.err" >&2
    bad "abi-verify manifest failed on $elf (exit $rc_man)"
fi
if [ -f "$fixture" ]; then
    cp "$fixture" "$work/fixture.json"
fi
rc_diff=123
python3 - "$work/sample.json" "$work/fixture.json" "$elf" <<'PY'
import json, sys
sample_p, fixture_p, elf = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    sample = json.load(open(sample_p))
except Exception as e:
    print(f"  [FAIL] cannot read sample manifest: {e}"); sys.exit(1)
if not (fixture_p and fixture_p.endswith('.json') and fixture_p != sample_p):
    print("  [info] no fixture to diff against"); sys.exit(0)
try:
    fix = json.load(open(fixture_p))
except Exception as e:
    print(f"  [FAIL] cannot read fixture {fixture_p}: {e}"); sys.exit(1)

def get(d, *path):
    for k in path:
        if not isinstance(d, dict) or k not in d: return None
        d = d[k]
    return d

rc = 0
def check(label, a, b, info_only=False):
    global rc
    if a == b:
        print(f"  [ok]   {label}: {a!r}")
    else:
        tag = "[info]" if info_only else "[FAIL]"
        if not info_only: rc = 1
        print(f"  {tag} {label}: sample={a!r} fixture={b!r}")

print("  ELF identity (common to relocatable + executable):")
for f in ("class","endian","machine"):
    check(f"elf.{f}", get(sample,"elf",f), get(fix,"elf",f))
print("  ELF type-dependent (reported, not gating REL vs EXEC):")
check("elf.os_abi", get(sample,"elf","os_abi"), get(fix,"elf","os_abi"), info_only=True)
check("elf.elf_type", get(sample,"elf","elf_type"), get(fix,"elf","elf_type"), info_only=True)
check("elf.flags", get(sample,"elf","flags"), get(fix,"elf","flags"))

print("  .opd entry shape (stride + head/tail reloc types):")
if get(sample,"opd") is None and get(fix,"opd") is None:
    print("  [ok]   .opd: absent in both")
elif get(sample,"opd") is None or get(fix,"opd") is None:
    print(f"  [FAIL] .opd present in one manifest only: sample={get(sample,'opd') is not None} fixture={get(fix,'opd') is not None}"); rc=1
else:
    check("opd.entry_size", get(sample,"opd","entry_size"), get(fix,"opd","entry_size"))
    def uniform_shape(d):
        # The invariant is the descriptor *shape*, not the descriptor count:
        # a startfile and a full executable legitimately have different
        # numbers of .opd descriptors, but a conforming one has all of them
        # the same shape.  Collapse to the set of distinct (head, tail) types.
        seen=set()
        for d2 in d:
            tail = get(d2,"tail_reloc","r_type") if get(d2,"tail_reloc") else None
            seen.add((get(d2,"head_reloc","r_type"), tail))
        return sorted(seen) if seen else None
    ss, fs = uniform_shape((get(sample,"opd","descriptors") or [])), uniform_shape((get(fix,"opd","descriptors") or []))
    check("opd descriptor shape set (head/tail reloc types)", ss, fs)

print("  .sys_proc_prx_param (size/magic/version):")
sp = get(sample,"sys_proc_prx_param"); fp = get(fix,"sys_proc_prx_param")
if sp is None and fp is None:
    print("  [ok]   .sys_proc_prx_param: absent in both")
elif sp is None or fp is None:
    print(f"  [FAIL] .sys_proc_prx_param present in one manifest only: sample={sp is not None} fixture={fp is not None}"); rc=1
else:
    for f in ("size","magic","version"):
        check(f"sys_proc_prx_param.{f}", get(sample,"sys_proc_prx_param",f), get(fix,"sys_proc_prx_param",f))

sys.exit(rc)
PY
rc_diff=$?
if [ "$rc_diff" -ne 0 ]; then
    bad "manifest diff vs fixture (see above)"
else
    ok "manifest matches the fixture on common fields"
fi

# ---------------------------------------------------------------- final verdict
printf '\n== %s ==\n' "abi-conformance verdict"
if [ "$fail" -eq 0 ]; then
    echo "PASS — section 7.1 (build + zero warnings + abi-verify check) and 7.2 (manifest vs fixture) hold."
    echo "     section 7.3 (run in RPCS3) remains a manual/emulator step."
    exit 0
else
    echo "FAIL — one or more section-7 invariants do not hold."
    echo "     Per the spec, a deviation requires either a fixture update with"
    echo "     documented rationale or a change to abi-verify with a regression test."
    exit 1
fi

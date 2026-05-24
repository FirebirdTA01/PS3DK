#!/usr/bin/env bash
# librt-bisect-harness.sh -- parameterized native-R1 vs PSL1GHT librt swap harness.
#
# This stays on the real build path:
#   - rebuilds librt through scripts/build-runtime-lv2.sh
#   - overlays selected .c sources from the PSL1GHT clean tree
#   - builds samples/sysutil/hello-ppu-msgdialog through CMake
#
# It never hand-archives a mixed librt.a and never launches RPCS3.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

native_librt_dir="$PS3_TOOLCHAIN_ROOT/runtime/lv2/librt"
psl1ght_librt_dir="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/librt"
psl1ght_archive_default="$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/librt/lib/ppu/librt.a"
sample_default="$PS3_TOOLCHAIN_ROOT/samples/sysutil/hello-ppu-msgdialog"
functional_diff_default="$PS3_TOOLCHAIN_ROOT/build/librt-functional-diff.txt"

usage() {
    cat >&2 <<EOF
Usage:
  $0 inventory [--psl1ght-archive PATH]
  $0 build [options]

Commands:
  inventory
      Print native R1 librt and PSL1GHT archive object inventories.

  build
      Rebuild librt through scripts/build-runtime-lv2.sh with selected
      objects swapped from PSL1GHT, then build hello-ppu-msgdialog
      through the real CMake pipeline.

Options for build:
  --workdir DIR          Use DIR as the scratch root (default: mktemp under /tmp)
  --sample DIR           Sample root to build (default: samples/sysutil/hello-ppu-msgdialog)
  --override-include DIR Header override root for the sample build
                         (must contain cell/sysutil.h and cell/msg_dialog.h)
  --psl1ght-object OBJ   Take OBJ from PSL1GHT instead of native R1 (repeatable)
  --psl1ght-list FILE    Read one OBJ per line from FILE; comments and blanks ignored
  --psl1ght-archive PATH PSL1GHT archive to inspect (default: $psl1ght_archive_default)
  --functional-list FILE Functionally divergent object manifest (default: build/librt-functional-diff.txt if present)
  --help                 Show this help

The build command treats everything as native R1 unless explicitly
swapped to PSL1GHT with --psl1ght-object / --psl1ght-list.
EOF
    exit 2
}

die() {
    printf '%s: ERROR: %s\n' "$(basename "$0")" "$*" >&2
    exit 1
}

list_native_objects() {
    find "$native_librt_dir" -maxdepth 1 -type f -name '*.c' -printf '%f\n' \
        | sed 's/\.c$/.o/' | sort
}

list_archive_objects() {
    local archive_path="$1"
    ar t "$archive_path" | sort
}

print_object_block() {
    local label="$1"
    local count="$2"
    shift 2
    printf '%s_count=%s\n' "$label" "$count"
    printf '%s_objects=\n' "$label"
    while IFS= read -r obj; do
        printf '%s\n' "$obj"
    done
}

compare_object_sets() {
    local left_file="$1"
    local right_file="$2"
    if diff -u "$left_file" "$right_file" >/dev/null; then
        return 0
    fi
    diff -u "$left_file" "$right_file"
    return 1
}

write_sorted_list() {
    local path="$1"
    shift
    : > "$path"
    if [[ $# -gt 0 ]]; then
        printf '%s\n' "$@" | sort > "$path"
    fi
}

archive_diff_objects() {
    local left_archive="$1"
    local right_archive="$2"
    local tmp_dir
    tmp_dir="$(mktemp -d)"
    mkdir -p "$tmp_dir/left" "$tmp_dir/right"
    (cd "$tmp_dir/left" && ar x "$left_archive")
    (cd "$tmp_dir/right" && ar x "$right_archive")

    local left_objs right_objs obj
    mapfile -t left_objs < <(cd "$tmp_dir/left" && printf '%s\n' *.o | sort)
    mapfile -t right_objs < <(cd "$tmp_dir/right" && printf '%s\n' *.o | sort)
    if ! diff -u <(printf '%s\n' "${left_objs[@]}") <(printf '%s\n' "${right_objs[@]}") >/dev/null; then
        diff -u <(printf '%s\n' "${left_objs[@]}") <(printf '%s\n' "${right_objs[@]}")
        rm -rf "$tmp_dir"
        return 1
    fi

    for obj in "${left_objs[@]}"; do
        if ! cmp -s "$tmp_dir/left/$obj" "$tmp_dir/right/$obj"; then
            printf '%s\n' "$obj"
        fi
    done | sort

    rm -rf "$tmp_dir"
}

copy_install_prefix() {
    local src_root="$1"
    local dst_root="$2"
    rm -rf "$dst_root"
    mkdir -p "$dst_root"
    cp -a "$src_root/." "$dst_root/"
}

swap_archive_objects() {
    local archive_path="$1"
    local source_archive="$2"
    shift 2
    local swap_dir="$1"
    shift
    local obj

    if [[ $# -eq 0 ]]; then
        return 0
    fi

    mkdir -p "$swap_dir"
    (cd "$swap_dir" && ar x "$source_archive" "$@")
    for obj in "$@"; do
        ar d "$archive_path" "$obj"
    done
    for obj in "$@"; do
        ar r "$archive_path" "$swap_dir/$obj"
    done
    ranlib "$archive_path"
}

populate_swap_set() {
    local -n out_swap_set="$1"
    shift
    local item
    for item in "$@"; do
        [[ -n "$item" ]] || continue
        out_swap_set["$item"]=1
    done
}

load_swap_list_file() {
    local path="$1"
    local -n out_swap_set="$2"
    local line item
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -n "$line" ]] || continue
        item="$line"
        out_swap_set["$item"]=1
    done < "$path"
}

build_overlay() {
    local overlay_dir="$1"
    shift
    local -n swap_map="$1"
    local src obj src_name

    mkdir -p "$overlay_dir"
    cp -a "$native_librt_dir/." "$overlay_dir/"

    for obj in "${!swap_map[@]}"; do
        src_name="${obj%.o}.c"
        [[ -f "$native_librt_dir/$src_name" ]] || die "native source missing for $obj"
        [[ -f "$psl1ght_librt_dir/$src_name" ]] || die "PSL1GHT source missing for $obj"
        cp -f "$psl1ght_librt_dir/$src_name" "$overlay_dir/$src_name"
    done

    # PSL1GHT's lv2errno.c uses a local <lv2errno.h> include. Keep the
    # matching header available only when that object is part of the swap
    # set so the rest of the overlay stays native-default.
    if [[ -n "${swap_map[lv2errno.o]+x}" ]]; then
        [[ -f "$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/include/sys/lv2errno.h" ]] \
            || die "PSL1GHT lv2errno header missing"
        cp -f "$PS3_TOOLCHAIN_ROOT/src/ps3dev/PSL1GHT/ppu/include/sys/lv2errno.h" \
            "$overlay_dir/lv2errno.h"
    fi
}

link_dir() {
    local src="$1"
    local dst="$2"
    mkdir -p "$(dirname "$dst")"
    rm -rf "$dst"
    ln -s "$src" "$dst"
}

prepare_temp_prefixes() {
    local temp_root="$1"
    local temp_ps3dev="$temp_root/ps3dev"
    local temp_ps3dk="$temp_ps3dev/ps3dk"
    local host_ps3dev="$2"
    local host_ps3dk="$3"

    mkdir -p "$temp_ps3dev" "$temp_ps3dk"

    link_dir "$host_ps3dev/bin" "$temp_ps3dev/bin"
    link_dir "$host_ps3dev/ppu/bin" "$temp_ps3dev/ppu/bin"
    link_dir "$host_ps3dev/spu/bin" "$temp_ps3dev/spu/bin"

    # GCC resolves its install prefix through /proc/self/exe, so the
    # symlinked compiler binary still searches the host stage tree unless
    # the harness supplies a relocatable GCC prefix.  Copy the small GCC
    # metadata/libgcc tree and target library tree into the temp prefix;
    # build-runtime-lv2.sh overwrites the arm-specific CRT/librt files
    # there before the sample link runs.  libexec is large and read-only,
    # so keep that as a symlink for cc1/collect2.
    mkdir -p "$temp_ps3dev/ppu/lib" "$temp_ps3dev/ppu/powerpc64-ps3-elf"
    [[ -d "$host_ps3dev/ppu/lib/gcc" ]] || die "host GCC support dir missing: $host_ps3dev/ppu/lib/gcc"
    [[ -d "$host_ps3dev/ppu/libexec" ]] || die "host GCC libexec dir missing: $host_ps3dev/ppu/libexec"
    [[ -d "$host_ps3dev/ppu/powerpc64-ps3-elf/lib" ]] || die "host target lib dir missing: $host_ps3dev/ppu/powerpc64-ps3-elf/lib"
    cp -a "$host_ps3dev/ppu/lib/gcc" "$temp_ps3dev/ppu/lib/gcc"
    cp -a "$host_ps3dev/ppu/powerpc64-ps3-elf/lib" "$temp_ps3dev/ppu/powerpc64-ps3-elf/lib"
    link_dir "$host_ps3dev/ppu/libexec" "$temp_ps3dev/ppu/libexec"
    link_dir "$host_ps3dev/ppu/powerpc64-ps3-elf/bin" "$temp_ps3dev/ppu/powerpc64-ps3-elf/bin"
    link_dir "$host_ps3dev/ppu/powerpc64-ps3-elf/include" "$temp_ps3dev/ppu/powerpc64-ps3-elf/include"

    link_dir "$host_ps3dev/bin" "$temp_ps3dk/bin"
    mkdir -p "$temp_ps3dk/ppu" "$temp_ps3dk/spu"
    if [[ -d "$host_ps3dk/ppu/lib" ]]; then
        mkdir -p "$temp_ps3dk/ppu/lib"
        cp -a "$host_ps3dk/ppu/lib/." "$temp_ps3dk/ppu/lib/" 2>/dev/null || true
    else
        mkdir -p "$temp_ps3dk/ppu/lib"
    fi
    if [[ -d "$host_ps3dk/spu/lib" ]]; then
        mkdir -p "$temp_ps3dk/spu/lib"
        cp -a "$host_ps3dk/spu/lib/." "$temp_ps3dk/spu/lib/" 2>/dev/null || true
    else
        mkdir -p "$temp_ps3dk/spu/lib"
    fi

    mkdir -p "$temp_ps3dk/ppu/include"
    if [[ -d "$host_ps3dk/ppu/include" ]]; then
        cp -a "$host_ps3dk/ppu/include/." "$temp_ps3dk/ppu/include/" 2>/dev/null || true
    fi

    mkdir -p "$temp_ps3dk/spu/include"
    if [[ -d "$host_ps3dk/spu/include" ]]; then
        cp -a "$host_ps3dk/spu/include/." "$temp_ps3dk/spu/include/" 2>/dev/null || true
    fi

    printf '%s\n%s\n' "$temp_ps3dev" "$temp_ps3dk"
}

print_inventory() {
    local psl1ght_archive="$1"
    local native_tmp="$2"
    local psl1ght_tmp="$3"

    local native_count psl1ght_count psl1ght_sha
    native_count="$(wc -l < "$native_tmp" | tr -d ' ')"
    psl1ght_count="$(wc -l < "$psl1ght_tmp" | tr -d ' ')"
    psl1ght_sha="$(sha256sum "$psl1ght_archive" | awk '{print $1}')"

    printf 'native_source_dir=%s\n' "$native_librt_dir"
    printf 'native_object_count=%s\n' "$native_count"
    cat "$native_tmp"
    printf 'psl1ght_archive=%s\n' "$psl1ght_archive"
    printf 'psl1ght_archive_sha256=%s\n' "$psl1ght_sha"
    printf 'psl1ght_archive_object_count=%s\n' "$psl1ght_count"
    cat "$psl1ght_tmp"
    if compare_object_sets "$native_tmp" "$psl1ght_tmp"; then
        printf 'object_sets_match=yes\n'
    else
        printf 'object_sets_match=no\n'
        return 1
    fi
}

cmd_inventory() {
    local psl1ght_archive="$psl1ght_archive_default"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --psl1ght-archive)
                [[ $# -ge 2 ]] || usage
                psl1ght_archive="$2"
                shift 2
                ;;
            --help)
                usage
                ;;
            *)
                usage
                ;;
        esac
    done

    [[ -f "$psl1ght_archive" ]] || die "PSL1GHT archive not found: $psl1ght_archive"

    local native_tmp psl1ght_tmp
    native_tmp="$(mktemp)"
    psl1ght_tmp="$(mktemp)"

    list_native_objects > "$native_tmp"
    list_archive_objects "$psl1ght_archive" > "$psl1ght_tmp"
    print_inventory "$psl1ght_archive" "$native_tmp" "$psl1ght_tmp"
    rm -f "$native_tmp" "$psl1ght_tmp"
}

cmd_build() {
    local workdir="${LRT_BISECT_WORKDIR:-}"
    local sample_dir="$sample_default"
    local override_include=""
    local psl1ght_archive="$psl1ght_archive_default"
    local functional_diff_manifest="$functional_diff_default"
    declare -A swap_set=()

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workdir)
                [[ $# -ge 2 ]] || usage
                workdir="$2"
                shift 2
                ;;
            --sample)
                [[ $# -ge 2 ]] || usage
                sample_dir="$2"
                shift 2
                ;;
            --override-include)
                [[ $# -ge 2 ]] || usage
                override_include="$2"
                shift 2
                ;;
            --psl1ght-archive)
                [[ $# -ge 2 ]] || usage
                psl1ght_archive="$2"
                shift 2
                ;;
            --functional-list)
                [[ $# -ge 2 ]] || usage
                functional_diff_manifest="$2"
                shift 2
                ;;
            --psl1ght-object)
                [[ $# -ge 2 ]] || usage
                swap_set["$2"]=1
                shift 2
                ;;
            --psl1ght-list)
                [[ $# -ge 2 ]] || usage
                load_swap_list_file "$2" swap_set
                shift 2
                ;;
            --help)
                usage
                ;;
            *)
                usage
                ;;
        esac
    done

    [[ -d "$sample_dir" ]] || die "sample directory not found: $sample_dir"
    [[ -f "$psl1ght_archive" ]] || die "PSL1GHT archive not found: $psl1ght_archive"
    if [[ -n "$functional_diff_manifest" && ! -f "$functional_diff_manifest" ]]; then
        functional_diff_manifest=""
    fi
    local sample_name
    sample_name="$(basename "$sample_dir")"

    local tmp_root
    if [[ -z "$workdir" ]]; then
        tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/librt-bisect.XXXXXX")"
    else
        tmp_root="$workdir"
        mkdir -p "$tmp_root"
    fi

    local base_root="$tmp_root/native-base"
    local base_install="$base_root/install"
    local arm_root="$tmp_root/arm"
    local arm_install="$arm_root/install"
    local sample_overlay="$tmp_root/sample-overlay"
    local override_overlay="$tmp_root/override-include"
    local build_dir="$tmp_root/sample-build"
    local native_tmp="$tmp_root/native-objects.txt"
    local psl1ght_tmp="$tmp_root/psl1ght-objects.txt"
    local expected_diff_tmp="$tmp_root/expected-diff.txt"
    local arm_diff_tmp="$tmp_root/arm-diff.txt"
    local swap_manifest="$tmp_root/swap-manifest.txt"

    printf 'workdir=%s\n' "$tmp_root"
    printf 'sample_dir=%s\n' "$sample_dir"
    printf 'psl1ght_archive=%s\n' "$psl1ght_archive"
    printf 'psl1ght_swap_count=%s\n' "${#swap_set[@]}"
    if [[ "${#swap_set[@]}" -gt 0 ]]; then
        printf 'psl1ght_swaps=\n'
        printf '%s\n' "${!swap_set[@]}" | sort
    fi

    list_native_objects > "$native_tmp"
    list_archive_objects "$psl1ght_archive" > "$psl1ght_tmp"
    print_inventory "$psl1ght_archive" "$native_tmp" "$psl1ght_tmp"

    local obj
    for obj in "${!swap_set[@]}"; do
        grep -qx "$obj" "$native_tmp" || die "unknown native librt object: $obj"
        grep -qx "$obj" "$psl1ght_tmp" || die "PSL1GHT archive does not contain: $obj"
    done

    declare -a swap_objs=()
    if [[ "${#swap_set[@]}" -gt 0 ]]; then
        mapfile -t swap_objs < <(printf '%s\n' "${!swap_set[@]}" | sort)
    fi
    write_sorted_list "$swap_manifest" "${swap_objs[@]}"
    printf 'swap_manifest=%s\n' "$swap_manifest"
    printf 'swap_objects=\n'
    cat "$swap_manifest"

    if [[ "$sample_name" == "hello-ppu-msgdialog" ]]; then
        if [[ -z "$override_include" ]]; then
            mkdir -p "$override_overlay/cell"
            cp -f "$PS3_TOOLCHAIN_ROOT/sdk/include/cell/sysutil.h" "$override_overlay/cell/sysutil.h"
            cp -f "$PS3_TOOLCHAIN_ROOT/sdk/include/cell/msg_dialog.h" "$override_overlay/cell/msg_dialog.h"
            override_include="$override_overlay"
        else
            [[ -d "$override_include/cell" ]] || die "override include directory must contain a cell/ subtree: $override_include"
        fi
    fi
    printf 'override_include=%s\n' "$override_include"

    mkdir -p "$sample_overlay"
    cp -a "$sample_dir/." "$sample_overlay/"
    if [[ "$sample_name" == "hello-ppu-msgdialog" && -f "$sample_overlay/CMakeLists.txt" ]]; then
        sed -i "/project(hello-ppu-msgdialog C)/a include_directories(BEFORE \"${override_include}\")" "$sample_overlay/CMakeLists.txt"
        perl -0pi -e 's/target_link_libraries\(\s*hello-ppu-msgdialog\s+PRIVATE\s+sysutil\s+rt\s+lv2\s*\)/target_link_libraries(hello-ppu-msgdialog PRIVATE\n    sysutil_stub sysutil rt lv2\n)/s' "$sample_overlay/CMakeLists.txt"
        if ! grep -q 'sysutil_stub' "$sample_overlay/CMakeLists.txt"; then
            die "failed to inject sysutil_stub into sample CMakeLists.txt"
        fi
        grep -q "include_directories(BEFORE" "$sample_overlay/CMakeLists.txt" \
            || die "failed to inject override include into sample CMakeLists.txt"
    fi
    printf 'sample_overlay=%s\n' "$sample_overlay"

    local host_ps3dev="$PS3DEV"
    local host_ps3dk="$PS3DK"
    mapfile -t base_prefixes < <(prepare_temp_prefixes "$base_install" "$host_ps3dev" "$host_ps3dk")
    local base_ps3dev="${base_prefixes[0]}"
    local base_ps3dk="${base_prefixes[1]}"
    printf 'base_ps3dev=%s\n' "$base_ps3dev"
    printf 'base_ps3dk=%s\n' "$base_ps3dk"
    printf 'sdk_install_cmd=%s\n' "env PS3DEV=$base_ps3dev PS3DK=$base_ps3dk make -C $PS3_TOOLCHAIN_ROOT/sdk install"
    env \
        PS3DEV="$base_ps3dev" \
        PS3DK="$base_ps3dk" \
        make -C "$PS3_TOOLCHAIN_ROOT/sdk" install
    local sdk_install_marker_file="$base_ps3dk/VERSION"
    local sdk_install_header_marker="$base_ps3dk/ppu/include/cell/sdk_version.h"
    printf 'sdk_install_marker_file=%s\n' "$sdk_install_marker_file"
    printf 'sdk_install_header_marker=%s\n' "$sdk_install_header_marker"
    [[ -f "$sdk_install_marker_file" ]] \
        || die "SDK install marker missing: $sdk_install_marker_file"
    [[ -f "$sdk_install_header_marker" ]] \
        || die "SDK install header marker missing: $sdk_install_header_marker"

    printf 'build_runtime_cmd=%s\n' "env PS3DEV=$base_ps3dev PS3DK=$base_ps3dk PSL1GHT=$base_ps3dk LIBRT_WORK_ROOT=$base_root/build/runtime-lv2/librt $PS3_TOOLCHAIN_ROOT/scripts/build-runtime-lv2.sh"
    env \
        PS3DEV="$base_ps3dev" \
        PS3DK="$base_ps3dk" \
        PSL1GHT="$base_ps3dk" \
        LIBRT_WORK_ROOT="$base_root/build/runtime-lv2/librt" \
        "$PS3_TOOLCHAIN_ROOT/scripts/build-runtime-lv2.sh"

    local base_librt="$base_ps3dev/ppu/powerpc64-ps3-elf/lib/librt.a"
    [[ -f "$base_librt" ]] || die "native librt build did not produce $base_librt"
    printf 'base_librt=%s\n' "$base_librt"
    printf 'base_librt_objects=\n'
    ar t "$base_librt" | sort

    if [[ -n "$functional_diff_manifest" ]]; then
        mapfile -t expected_diff < "$functional_diff_manifest"
    else
        mapfile -t expected_diff < <(archive_diff_objects "$base_librt" "$psl1ght_archive")
    fi
    write_sorted_list "$expected_diff_tmp" "${expected_diff[@]}"
    printf 'expected_diff_count=%s\n' "$(wc -l < "$expected_diff_tmp" | tr -d ' ')"
    printf 'expected_diff_objects=\n'
    cat "$expected_diff_tmp"

    copy_install_prefix "$base_install" "$arm_install"
    printf 'arm_ps3dev=%s\n' "$arm_install/ps3dev"
    printf 'arm_ps3dk=%s\n' "$arm_install/ps3dev/ps3dk"

    local arm_librt="$arm_install/ps3dev/ppu/powerpc64-ps3-elf/lib/librt.a"
    local swap_extract="$tmp_root/swap-extract"
    if [[ "${#swap_objs[@]}" -gt 0 ]]; then
        swap_archive_objects "$arm_librt" "$psl1ght_archive" "$swap_extract" "${swap_objs[@]}"
    fi
    [[ -f "$arm_librt" ]] || die "arm librt archive missing: $arm_librt"
    printf 'arm_librt=%s\n' "$arm_librt"
    printf 'arm_librt_objects=\n'
    ar t "$arm_librt" | sort

    mapfile -t arm_diff < <(archive_diff_objects "$arm_librt" "$base_librt")
    write_sorted_list "$arm_diff_tmp" "${arm_diff[@]}"
    printf 'arm_vs_base_diff_count=%s\n' "$(wc -l < "$arm_diff_tmp" | tr -d ' ')"
    printf 'arm_vs_base_diff_objects=\n'
    cat "$arm_diff_tmp"

    local arm_gcc="$arm_install/ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
    local arm_gcc_exec_prefix="$arm_install/ps3dev/ppu/lib/gcc/"
    local arm_resolved_librt
    arm_resolved_librt="$(env GCC_EXEC_PREFIX="$arm_gcc_exec_prefix" "$arm_gcc" -mcpu=cell -print-file-name=librt.a)"
    printf 'arm_gcc_exec_prefix=%s\n' "$arm_gcc_exec_prefix"
    printf 'arm_gcc_resolved_librt=%s\n' "$arm_resolved_librt"
    [[ "$(realpath "$arm_resolved_librt")" == "$(realpath "$arm_librt")" ]] \
        || die "arm GCC resolves librt outside arm prefix: $arm_resolved_librt"

    mkdir -p "$build_dir"
    printf 'sample_build_dir=%s\n' "$build_dir"
    printf 'cmake_configure_cmd=%s\n' "env PS3DEV=$arm_install/ps3dev PS3DK=$arm_install/ps3dev/ps3dk GCC_EXEC_PREFIX=$arm_gcc_exec_prefix cmake -S $sample_overlay -B $build_dir -DCMAKE_TOOLCHAIN_FILE=$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"
    env \
        PS3DEV="$arm_install/ps3dev" \
        PS3DK="$arm_install/ps3dev/ps3dk" \
        GCC_EXEC_PREFIX="$arm_gcc_exec_prefix" \
        cmake -S "$sample_overlay" -B "$build_dir" \
            -DCMAKE_TOOLCHAIN_FILE="$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"

    printf 'cmake_build_cmd=%s\n' "env PS3DEV=$arm_install/ps3dev PS3DK=$arm_install/ps3dev/ps3dk GCC_EXEC_PREFIX=$arm_gcc_exec_prefix cmake --build $build_dir"
    env \
        PS3DEV="$arm_install/ps3dev" \
        PS3DK="$arm_install/ps3dev/ps3dk" \
        GCC_EXEC_PREFIX="$arm_gcc_exec_prefix" \
        cmake --build "$build_dir"

    printf 'sample_elf=%s\n' "$build_dir/hello-ppu-msgdialog"
    printf 'sample_self=%s\n' "$sample_overlay/hello-ppu-msgdialog.self"
    printf 'sample_fake_self=%s\n' "$sample_overlay/hello-ppu-msgdialog.fake.self"
}

main() {
    [[ $# -ge 1 ]] || usage
    case "$1" in
        inventory)
            shift
            cmd_inventory "$@"
            ;;
        build)
            shift
            cmd_build "$@"
            ;;
        --help|-h|help)
            usage
            ;;
        *)
            usage
            ;;
    esac
}

main "$@"

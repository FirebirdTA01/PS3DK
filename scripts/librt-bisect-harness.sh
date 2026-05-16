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

    local tmp_root
    if [[ -z "$workdir" ]]; then
        tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/librt-bisect.XXXXXX")"
    else
        tmp_root="$workdir"
        mkdir -p "$tmp_root"
    fi

    local overlay_dir="$tmp_root/overlay/runtime/lv2/librt"
    local sample_overlay="$tmp_root/sample-overlay"
    local override_overlay="$tmp_root/override-include"
    local install_root="$tmp_root/install"
    local temp_ps3dev="$install_root/ps3dev"
    local temp_ps3dk="$temp_ps3dev/ps3dk"
    local build_dir="$tmp_root/sample-build"
    local native_tmp="$tmp_root/native-objects.txt"
    local psl1ght_tmp="$tmp_root/psl1ght-objects.txt"

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

    build_overlay "$overlay_dir" swap_set
    printf 'overlay_dir=%s\n' "$overlay_dir"

    if [[ -z "$override_include" ]]; then
        mkdir -p "$override_overlay/cell"
        cp -f "$PS3_TOOLCHAIN_ROOT/sdk/include/cell/sysutil.h" "$override_overlay/cell/sysutil.h"
        cp -f "$PS3_TOOLCHAIN_ROOT/sdk/include/cell/msg_dialog.h" "$override_overlay/cell/msg_dialog.h"
        override_include="$override_overlay"
    else
        [[ -d "$override_include/cell" ]] || die "override include directory must contain a cell/ subtree: $override_include"
    fi
    printf 'override_include=%s\n' "$override_include"

    mkdir -p "$sample_overlay"
    cp -a "$sample_dir/." "$sample_overlay/"
    if [[ -f "$sample_overlay/CMakeLists.txt" ]]; then
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
    mapfile -t temp_prefixes < <(prepare_temp_prefixes "$install_root" "$host_ps3dev" "$host_ps3dk")
    local temp_ps3dev="${temp_prefixes[0]}"
    local temp_ps3dk="${temp_prefixes[1]}"
    printf 'temp_ps3dev=%s\n' "$temp_ps3dev"
    printf 'temp_ps3dk=%s\n' "$temp_ps3dk"

    printf 'build_runtime_cmd=%s\n' "env PS3DEV=$temp_ps3dev PS3DK=$temp_ps3dk PSL1GHT=$temp_ps3dk LIBRT_SRC_DIR=$overlay_dir $PS3_TOOLCHAIN_ROOT/scripts/build-runtime-lv2.sh"
    env \
        PS3DEV="$temp_ps3dev" \
        PS3DK="$temp_ps3dk" \
        PSL1GHT="$temp_ps3dk" \
        LIBRT_SRC_DIR="$overlay_dir" \
        "$PS3_TOOLCHAIN_ROOT/scripts/build-runtime-lv2.sh"

    local built_librt="$temp_ps3dev/ppu/powerpc64-ps3-elf/lib/librt.a"
    [[ -f "$built_librt" ]] || die "native librt build did not produce $built_librt"
    printf 'built_librt=%s\n' "$built_librt"
    printf 'built_librt_objects=\n'
    ar t "$built_librt" | sort

    mkdir -p "$build_dir"
    printf 'sample_build_dir=%s\n' "$build_dir"
    printf 'cmake_configure_cmd=%s\n' "env PS3DEV=$temp_ps3dev PS3DK=$temp_ps3dk cmake -S $sample_overlay -B $build_dir -DCMAKE_TOOLCHAIN_FILE=$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"
    env \
        PS3DEV="$temp_ps3dev" \
        PS3DK="$temp_ps3dk" \
        cmake -S "$sample_overlay" -B "$build_dir" \
            -DCMAKE_TOOLCHAIN_FILE="$PS3_TOOLCHAIN_ROOT/cmake/ps3-ppu-toolchain.cmake"

    printf 'cmake_build_cmd=%s\n' "env PS3DEV=$temp_ps3dev PS3DK=$temp_ps3dk cmake --build $build_dir"
    env \
        PS3DEV="$temp_ps3dev" \
        PS3DK="$temp_ps3dk" \
        cmake --build "$build_dir"

    printf 'sample_elf=%s\n' "$build_dir/hello-ppu-msgdialog"
    printf 'sample_self=%s\n' "$build_dir/hello-ppu-msgdialog.self"
    printf 'sample_fake_self=%s\n' "$build_dir/hello-ppu-msgdialog.fake.self"
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

# PS3 Custom Toolchain — CMake helper for the PPU strip → sprxlinker
# → make_self / fself post-build pipeline.
#
# Sample usage (in a sample CMakeLists.txt):
#
#     include(ps3-self)
#
#     add_executable(my_app source/main.cpp)
#     target_link_libraries(my_app PRIVATE sysutil m)
#
#     ps3_add_self(my_app)
#
# Produces (in the build directory):
#     my_app                # raw .elf (CMake's executable target)
#     my_app.stripped.elf   # post-strip + sprxlinker
#     my_app.self           # CEX-signed (boots in RPCS3 / signed HW)
#     my_app.fake.self      # fake-signed (boots in CFW / ps3load)
#
# All four artefacts land in the same directory as the CMake target's
# binary output (i.e. the build dir, not source).  Match the existing
# Makefile-driven sample's ${TARGET}.{self,fake.self} convention.

include_guard(GLOBAL)

# -----------------------------------------------------------------------------
# One-time host-tool probe
# -----------------------------------------------------------------------------
if(NOT _PS3_SELF_TOOLS_PROBED)
    set(_PS3_SELF_TOOLS_PROBED TRUE)

    set(_ps3_self_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_self_exe ".exe")
    endif()

    # Locate the four host tools.  $PS3DEV/bin (Linux native install)
    # is checked first; $PS3DK/bin (Windows release zip) is the
    # fallback.  NO_DEFAULT_PATH is critical here — sprxlinker /
    # make_self / fself are not standard system binaries, and a
    # find-anywhere search would just confuse diagnostics if they're
    # missing.
    foreach(tool sprxlinker make_self make_self_npdrm fself)
        find_program(PS3_TOOL_${tool}
            NAMES "${tool}${_ps3_self_exe}" "${tool}"
            PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
            NO_DEFAULT_PATH)
        if(NOT PS3_TOOL_${tool})
            message(FATAL_ERROR
                "ps3-self: required host tool '${tool}${_ps3_self_exe}' not found.\n"
                "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.\n"
                "  Run scripts/build-psl1ght.sh (Linux) or extract the Windows release zip.")
        endif()
    endforeach()
endif()

# -----------------------------------------------------------------------------
# ps3_add_self(target [TITLE str] [APPID str] [CONTENTID str])
# -----------------------------------------------------------------------------
# TITLE / APPID / CONTENTID are reserved for the .pkg target which a
# subsequent phase (7c+) will wire up via make_self_npdrm + sfo.py +
# pkg.py + package_finalize.  For the MVP the function only emits
# the .self / .fake.self post-build chain.
function(ps3_add_self target)
    cmake_parse_arguments(_PSA "" "TITLE;APPID;CONTENTID" "" ${ARGN})

    if(_PSA_UNPARSED_ARGUMENTS)
        message(WARNING "ps3_add_self: unrecognised arguments ignored: ${_PSA_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_self: target '${target}' does not exist")
    endif()

    # Resolve the .elf path via $<TARGET_FILE:..> at build time, but
    # use the target's RUNTIME_OUTPUT_DIRECTORY (or the binary dir as
    # fallback) for the post-build artefacts so BYPRODUCTS can be a
    # plain string — generator expressions in BYPRODUCTS are accepted
    # but the support is uneven across CMake versions and Ninja.
    get_target_property(_outdir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _outdir)
        set(_outdir "${CMAKE_BINARY_DIR}")
    endif()

    set(_elf       "$<TARGET_FILE:${target}>")
    set(_stripped  "${_outdir}/${target}.stripped.elf")
    set(_self      "${_outdir}/${target}.self")
    set(_fake_self "${_outdir}/${target}.fake.self")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_STRIP}" "${_elf}" -o "${_stripped}"
        COMMAND "${PS3_TOOL_sprxlinker}" "${_stripped}"
        COMMAND "${PS3_TOOL_make_self}"  "${_stripped}" "${_self}"
        COMMAND "${PS3_TOOL_fself}"      "${_stripped}" "${_fake_self}"
        BYPRODUCTS "${_stripped}" "${_self}" "${_fake_self}"
        COMMENT "ps3-self: ${target}.{self,fake.self}"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# bin2s tool probe + ps3_bin2s(target file)
# -----------------------------------------------------------------------------
# bin2s converts a binary file into a .s assembly source (with byte
# data + extern symbols) plus a generated .h declaring three externs:
# <id>[], <id>_end[], <id>_size.  The generated .o is then linked
# into the target like any other source file, so the C/C++ code can
# `#include "<id>.h"` and reference the symbols directly.
#
# ID derivation matches PSL1GHT's data_rules: replace dots with
# underscores and prefix a leading-digit name with `_` (so `9.png`
# becomes `_9_png`).
if(NOT _PS3_BIN2S_PROBED)
    set(_PS3_BIN2S_PROBED TRUE)
    set(_ps3_self_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_self_exe ".exe")
    endif()
    find_program(PS3_TOOL_bin2s
        NAMES "bin2s${_ps3_self_exe}" "bin2s"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
        NO_DEFAULT_PATH)
    # bin2s is optional — only samples that embed binary data need it.
    # We don't FATAL_ERROR if it's missing; ps3_bin2s will surface a
    # clear error if anyone calls it.
endif()

# Path to the cmake -P implementation script — runs in a child CMake
# process so we can use execute_process(... OUTPUT_FILE ...) and
# file(WRITE ...) cross-platform without resorting to bash -c / shell
# redirection.
set(_PS3_BIN2S_IMPL "${CMAKE_CURRENT_LIST_DIR}/ps3-bin2s-impl.cmake")

function(ps3_bin2s target file)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_bin2s: target '${target}' does not exist")
    endif()
    if(NOT PS3_TOOL_bin2s)
        message(FATAL_ERROR "ps3_bin2s: bin2s host tool not found in ${PS3DEV}/bin or ${PS3DK}/bin")
    endif()

    # Resolve absolute path of the input file (relative to source dir
    # if not already absolute).
    if(IS_ABSOLUTE "${file}")
        set(_input "${file}")
    else()
        set(_input "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
    endif()
    if(NOT EXISTS "${_input}")
        # File may be a build-time generated artifact (e.g. an SPU
        # ELF produced by ps3_add_spu_image).  Don't FATAL_ERROR at
        # configure time; the dependency arrow will surface a clear
        # error at build time if the source genuinely doesn't exist.
        # Still skip the EXISTS check on absolute generated paths.
    endif()

    get_filename_component(_basename "${_input}" NAME)
    string(REGEX REPLACE "^([0-9])" "_\\1" _id "${_basename}")
    string(REPLACE "." "_" _id "${_id}")

    set(_outdir "${CMAKE_CURRENT_BINARY_DIR}")
    set(_s   "${_outdir}/${_basename}.s")
    set(_hdr "${_outdir}/${_id}.h")

    add_custom_command(
        OUTPUT  "${_s}" "${_hdr}"
        COMMAND "${CMAKE_COMMAND}"
                "-DPS3_BIN2S_TOOL=${PS3_TOOL_bin2s}"
                "-DPS3_BIN2S_INPUT=${_input}"
                "-DPS3_BIN2S_S=${_s}"
                "-DPS3_BIN2S_HDR=${_hdr}"
                "-DPS3_BIN2S_ID=${_id}"
                -P "${_PS3_BIN2S_IMPL}"
        DEPENDS "${_input}"
        COMMENT "ps3-bin2s: ${_basename} -> ${_id}.{s,h}"
        VERBATIM)

    target_sources(${target} PRIVATE "${_s}")
    target_include_directories(${target} PRIVATE "${_outdir}")
endfunction()

# -----------------------------------------------------------------------------
# ps3_add_spu_image(target NAME <name> SOURCES <files...> [LIBS <libs...>])
# -----------------------------------------------------------------------------
# Compiles SPU sources via $PS3DEV/spu/bin/spu-elf-gcc into a single
# spu/${NAME}.elf, then bin2s-embeds that ELF into <target> (the PPU
# executable).  PPU code can `#include "${NAME}_elf.h"` and reference
# the symbols (${NAME}_elf, ${NAME}_elf_end, ${NAME}_elf_size) to
# pass to sys_spu_image_import.
#
# This avoids the recursive-cmake / ExternalProject_Add dance —
# SPU compilation is small (typically 1-3 files) and the spu-elf
# toolchain is in a known location alongside the PPU one, so we
# invoke it directly with add_custom_command.  Flags mirror the
# spu_rules MACHDEP defaults (-Os, -fpic, -fno-exceptions / -fno-rtti
# for C++).
#
# SOURCES paths are resolved against CMAKE_CURRENT_SOURCE_DIR.  LIBS
# are -l-style names that exist in $PS3DEV/spu/powerpc-..-lib or
# $PS3DK/spu/lib (e.g. simdmath, sputhread).  The SPU link command
# adds -L for both directories automatically.
if(NOT _PS3_SPU_TOOLS_PROBED)
    set(_PS3_SPU_TOOLS_PROBED TRUE)
    set(_ps3_self_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_self_exe ".exe")
    endif()
    find_program(PS3_SPU_GCC
        NAMES "spu-elf-gcc${_ps3_self_exe}" "spu-elf-gcc"
        PATHS "${PS3DEV}/spu/bin"
        NO_DEFAULT_PATH)
    # Optional — only samples that embed SPU code need the SPU compiler.
endif()

function(ps3_add_spu_image target)
    cmake_parse_arguments(_PSI
        "NOSTARTFILES;FREESTANDING"            # boolean flags
        "NAME;LDSCRIPT"                         # single-value
        "SOURCES;LIBS;CFLAGS;LDFLAGS"           # multi-value
        ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_spu_image: target '${target}' does not exist")
    endif()
    if(NOT _PSI_NAME)
        message(FATAL_ERROR "ps3_add_spu_image: NAME is required")
    endif()
    if(NOT _PSI_SOURCES)
        message(FATAL_ERROR "ps3_add_spu_image: SOURCES is required")
    endif()
    if(NOT PS3_SPU_GCC)
        message(FATAL_ERROR "ps3_add_spu_image: spu-elf-gcc not found at ${PS3DEV}/spu/bin")
    endif()

    set(_spu_dir "${CMAKE_CURRENT_BINARY_DIR}/spu/${_PSI_NAME}")
    file(MAKE_DIRECTORY "${_spu_dir}")
    # Output name uses the .bin extension so bin2s emits symbols
    # that match the existing Makefile-driven convention
    # (<NAME>_bin / <NAME>_bin_end / <NAME>_bin_size + <NAME>_bin.h).
    # PPU code that does `#include "<NAME>_bin.h"` keeps working
    # without source edits.
    set(_spu_elf "${_spu_dir}/${_PSI_NAME}.bin")

    # SPU compile flags.  Defaults match PSL1GHT spu_rules MACHDEP
    # (code-size, position-independent, no C++ EH/RTTI).  Caller can
    # extend via CFLAGS or replace the freestanding/-fpic posture
    # entirely with FREESTANDING.
    set(_spu_cflags -Os -Wall -ffunction-sections -fdata-sections)
    if(_PSI_FREESTANDING)
        list(APPEND _spu_cflags -ffreestanding -fno-exceptions)
    else()
        list(APPEND _spu_cflags -fpic -fno-exceptions -fno-rtti)
    endif()
    list(APPEND _spu_cflags "-I${PS3DK}/spu/include" ${_PSI_CFLAGS})

    # Compile each source -> .o via add_custom_command
    set(_spu_objs)
    foreach(src ${_PSI_SOURCES})
        if(IS_ABSOLUTE "${src}")
            set(_in "${src}")
        else()
            set(_in "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
        endif()
        get_filename_component(_in_name "${src}" NAME)
        set(_out "${_spu_dir}/${_in_name}.o")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${PS3_SPU_GCC}" ${_spu_cflags} -c "${_in}" -o "${_out}"
            DEPENDS "${_in}"
            COMMENT "ps3-spu: ${_PSI_NAME}/${_in_name}"
            VERBATIM)
        list(APPEND _spu_objs "${_out}")
    endforeach()

    # Link flags + libs
    set(_spu_link_flags)
    if(NOT _PSI_FREESTANDING)
        list(APPEND _spu_link_flags -fpic)
    endif()
    list(APPEND _spu_link_flags -Wl,--gc-sections "-L${PS3DK}/spu/lib" ${_PSI_LDFLAGS})
    if(_PSI_NOSTARTFILES)
        list(APPEND _spu_link_flags -nostartfiles)
    endif()
    set(_link_deps ${_spu_objs})
    if(_PSI_LDSCRIPT)
        list(APPEND _spu_link_flags "-T" "${_PSI_LDSCRIPT}")
        list(APPEND _link_deps "${_PSI_LDSCRIPT}")
    endif()
    set(_spu_libs)
    foreach(lib ${_PSI_LIBS})
        list(APPEND _spu_libs "-l${lib}")
    endforeach()

    add_custom_command(
        OUTPUT "${_spu_elf}"
        COMMAND "${PS3_SPU_GCC}"
                ${_spu_link_flags}
                ${_spu_objs} ${_spu_libs}
                -o "${_spu_elf}"
        DEPENDS ${_link_deps}
        COMMENT "ps3-spu: link ${_PSI_NAME}.elf"
        VERBATIM)

    # Embed the SPU ELF into the PPU target via bin2s.  Symbol
    # prefix derives from the basename: "<NAME>.bin" → "<NAME>_bin".
    ps3_bin2s(${target} "${_spu_elf}")
endfunction()

# -----------------------------------------------------------------------------
# Cg shader compilation: .vcg / .fcg → .vpo / .fpo → bin2s-embedded
# -----------------------------------------------------------------------------
# ps3_add_cg_shader(<target> <file>)
#
# Compiles a Cg shader through the cgcomp host tool (PSL1GHT-installed
# at $PS3DEV/bin/cgcomp) and embeds the resulting compiled-shader
# blob (.vpo for vertex / .fpo for fragment) into the PPU target via
# bin2s.  PPU code references the shader via the bin2s symbol set:
#
#   `vpshader.vcg` → bin2s output `vpshader_vpo[]` etc., header
#                    `vpshader_vpo.h`
#   `fpshader.fcg` → bin2s output `fpshader_fpo[]` etc., header
#                    `fpshader_fpo.h`
#
# Profile is auto-detected from the file extension: .vcg → -v
# (vertex), .fcg → -f (fragment).  rsx-cg-compiler is on the
# longer-term roadmap as a drop-in replacement for cgcomp; until
# every test shader is byte-identical between the two, cgcomp stays
# the default here.
if(NOT _PS3_CG_PROBED)
    set(_PS3_CG_PROBED TRUE)
    set(_ps3_self_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_self_exe ".exe")
    endif()
    find_program(PS3_TOOL_cgcomp
        NAMES "cgcomp${_ps3_self_exe}" "cgcomp"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
        NO_DEFAULT_PATH)
    # cgcomp is optional — only Cg-shader-using samples need it.
endif()

function(ps3_add_cg_shader target file)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_cg_shader: target '${target}' does not exist")
    endif()
    if(NOT PS3_TOOL_cgcomp)
        message(FATAL_ERROR "ps3_add_cg_shader: cgcomp not found at ${PS3DEV}/bin or ${PS3DK}/bin")
    endif()

    if(IS_ABSOLUTE "${file}")
        set(_input "${file}")
    else()
        set(_input "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
    endif()
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "ps3_add_cg_shader: input file does not exist: ${_input}")
    endif()

    # Detect profile from extension.
    get_filename_component(_ext "${_input}" EXT)
    get_filename_component(_stem "${_input}" NAME_WE)
    if(_ext STREQUAL ".vcg")
        set(_profile_arg "-v")
        set(_out_ext "vpo")
    elseif(_ext STREQUAL ".fcg")
        set(_profile_arg "-f")
        set(_out_ext "fpo")
    else()
        message(FATAL_ERROR "ps3_add_cg_shader: ${file} has unrecognised extension ${_ext} (expected .vcg or .fcg)")
    endif()

    set(_outdir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${_outdir}")
    set(_compiled "${_outdir}/${_stem}.${_out_ext}")

    add_custom_command(
        OUTPUT  "${_compiled}"
        COMMAND "${PS3_TOOL_cgcomp}" "${_profile_arg}" "${_input}" "${_compiled}"
        DEPENDS "${_input}"
        COMMENT "ps3-cg: ${_stem}.${_ext} → ${_stem}.${_out_ext}"
        VERBATIM)

    # Embed the compiled shader blob into the target.  bin2s names
    # the symbols off the file basename — `<stem>.<out_ext>` becomes
    # `<stem>_<out_ext>` per the dot-to-underscore convention.
    ps3_bin2s(${target} "${_compiled}")
endfunction()

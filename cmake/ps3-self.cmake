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
        message(FATAL_ERROR "ps3_bin2s: input file does not exist: ${_input}")
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

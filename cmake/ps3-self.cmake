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

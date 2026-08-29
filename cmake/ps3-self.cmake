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
# Produces:
#     <build>/my_app          # raw unstripped .elf  (CMake's executable target)
#     <source>/my_app.elf     # stripped + sprxlinker-rewritten .elf
#     <source>/my_app.self    # CEX-signed (boots in RPCS3 / signed HW)
#     <source>/my_app.fake.self  # fake-signed (boots in CFW / ps3load)
#
# Final artefacts (.elf, .self, .fake.self) land at the sample source
# directory — matching the legacy `make`-driven convention where the
# stripped .elf and signed .self / .fake.self sat next to the
# Makefile.  The unstripped CMake-target binary stays in the build
# dir as an intermediate (and isn't useful on its own — it lacks the
# sprxlinker post-link rewrite that LV2 expects).
#
# Sample dirs already gitignore *.elf / *.self / *.fake.self so the
# generated files never show up in `git status`.

include_guard(GLOBAL)

# Captured once at file load so functions defined below resolve their
# default asset / template paths against the toolchain root rather
# than against the caller's CMakeLists dir.  CMAKE_CURRENT_LIST_DIR
# inside a function body evaluates lazily at the call site.
set(_PS3_SELF_CMAKE_DIR    "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(_PS3_TOOLCHAIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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
# Optional PRX host-tool probe
# -----------------------------------------------------------------------------
if(NOT _PS3_PRX_TOOLS_PROBED)
    set(_PS3_PRX_TOOLS_PROBED TRUE)

    set(_ps3_prx_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_prx_exe ".exe")
    endif()

    find_program(PS3_TOOL_prx_gen
        NAMES "prx-gen${_ps3_prx_exe}" "prx-gen"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
        NO_DEFAULT_PATH)
    find_program(PS3_TOOL_nidgen
        NAMES "nidgen${_ps3_prx_exe}" "nidgen"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
        NO_DEFAULT_PATH)
    find_program(PS3_TOOL_make_sprx
        NAMES "make_sprx${_ps3_prx_exe}" "make_sprx"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
        NO_DEFAULT_PATH)
endif()

# -----------------------------------------------------------------------------
# One-time installed-SDK freshness probe
# -----------------------------------------------------------------------------
if(NOT _PS3_SELF_SDK_INSTALL_PROBED)
    set(_PS3_SELF_SDK_INSTALL_PROBED TRUE)

    # Required artifacts come from ps3-required-artifacts.txt, the single
    # source of truth shared with scripts/package-windows-release.sh's
    # payload validation.  The two lists drifted before -- the packager
    # validated only flat ppu/lib stubs while this probe also required the
    # lp64 variants, liblv2.a, librsx.a and cell/sdk_version.h -- so a
    # release could validate clean and then fail here at configure time.
    # Do not re-inline this list.
    set(_ps3_manifest_file "${_PS3_SELF_CMAKE_DIR}/ps3-required-artifacts.txt")
    if(NOT EXISTS "${_ps3_manifest_file}")
        message(FATAL_ERROR
            "ps3-self: required-artifact manifest missing: ${_ps3_manifest_file}")
    endif()
    file(STRINGS "${_ps3_manifest_file}" _ps3_sdk_core_lines REGEX "^sdk_core[ 	]+")
    if(NOT _ps3_sdk_core_lines)
        message(FATAL_ERROR
            "ps3-self: no sdk_core entries parsed from ${_ps3_manifest_file}")
    endif()
    foreach(_line IN LISTS _ps3_sdk_core_lines)
        string(REGEX REPLACE "^sdk_core[ 	]+" "" _rel "${_line}")
        string(STRIP "${_rel}" _rel)
        if(NOT EXISTS "${PS3DK}/${_rel}")
            message(FATAL_ERROR
                "ps3-self: required SDK artifact missing: ${PS3DK}/${_rel}"
                "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
        endif()
    endforeach()

    # Stub aliases, checked on EVERY host.  sdk/Makefile installs them as
    # symlinks; the Windows packager materializes them into real copies because
    # Explorer and 7-Zip cannot restore NTFS symlinks.  Accept either shape here
    # -- the Unix block below additionally enforces strict symlink semantics for
    # source-tree installs.  Before this ran on Windows, a broken alias produced
    # a bare ld error with nothing pointing at the cause.
    file(STRINGS "${_ps3_manifest_file}" _ps3_alias_lines REGEX "^alias[ 	]+")
    foreach(_line IN LISTS _ps3_alias_lines)
        string(REGEX REPLACE "^alias[ 	]+" "" _row "${_line}")
        string(REGEX MATCH "^[^ 	]+" _rel "${_row}")
        string(REGEX REPLACE "^[^ 	]+[ 	]+" "" _tgt "${_row}")
        string(STRIP "${_tgt}" _tgt)
        get_filename_component(_adir "${PS3DK}/${_rel}" DIRECTORY)
        if(NOT EXISTS "${PS3DK}/${_rel}")
            message(FATAL_ERROR
                "ps3-self: required stub alias missing: ${PS3DK}/${_rel} (expected ${_tgt})"
                "  Reinstall the SDK, or re-extract the release zip.")
        endif()
        # SHA-compare unconditionally rather than skipping symlinks: file(SHA256)
        # reads THROUGH a symlink, so one code path covers the packaged copy, a
        # correct symlink, AND a symlink pointing at the wrong file -- which an
        # IS_SYMLINK early-out would have waved through.  The Unix block below
        # separately enforces that source-tree installs use real symlinks.
        if(NOT EXISTS "${_adir}/${_tgt}")
            message(FATAL_ERROR
                "ps3-self: stub alias target missing: ${_adir}/${_tgt}")
        endif()
        file(SHA256 "${PS3DK}/${_rel}" _ps3_alias_sha)
        file(SHA256 "${_adir}/${_tgt}" _ps3_target_sha)
        if(NOT _ps3_alias_sha STREQUAL _ps3_target_sha)
            message(FATAL_ERROR
                "ps3-self: stub alias ${_rel} does not match its target ${_tgt}."
                "  Expected a symlink to it or a byte-identical copy.  A ~17-byte alias"
                "  here means the zip was extracted by a tool that could not restore"
                "  symlinks; re-extract with the packaged release, or reinstall the SDK.")
        endif()
    endforeach()

    if(CMAKE_HOST_UNIX)
        set(_ps3_manifest "${PS3DK}/.ps3dk-install-manifest")
        if(NOT EXISTS "${_ps3_manifest}")
            message(FATAL_ERROR
                "ps3-self: SDK install manifest missing: ${_ps3_manifest}\n"
                "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
        endif()

        execute_process(
            COMMAND "${_PS3_TOOLCHAIN_ROOT}/scripts/version.sh" --format=plain
            WORKING_DIRECTORY "${_PS3_TOOLCHAIN_ROOT}"
            OUTPUT_VARIABLE _ps3_expected_version
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _ps3_version_rc)
        if(NOT _ps3_version_rc EQUAL 0)
            message(FATAL_ERROR "ps3-self: failed to compute source SDK version")
        endif()

        if(NOT EXISTS "${PS3DK}/VERSION")
            message(FATAL_ERROR
                "ps3-self: SDK VERSION marker missing: ${PS3DK}/VERSION\n"
                "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
        endif()
        file(READ "${PS3DK}/VERSION" _ps3_installed_version)
        string(STRIP "${_ps3_installed_version}" _ps3_installed_version)
        if(NOT _ps3_installed_version STREQUAL _ps3_expected_version)
            message(FATAL_ERROR
                "ps3-self: stale SDK install.\n"
                "  source:    ${_ps3_expected_version}\n"
                "  installed: ${_ps3_installed_version}\n"
                "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
        endif()

        set(_ps3_source_video_header "${_PS3_TOOLCHAIN_ROOT}/sdk/include/sysutil/video.h")
        set(_ps3_installed_video_header "${PS3DK}/ppu/include/sysutil/video.h")
        foreach(path "${_ps3_source_video_header}" "${_ps3_installed_video_header}")
            if(NOT EXISTS "${path}")
                message(FATAL_ERROR
                    "ps3-self: required SDK header missing: ${path}\n"
                    "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
            endif()
        endforeach()
        file(SHA256 "${_ps3_source_video_header}" _ps3_source_video_sha)
        file(SHA256 "${_ps3_installed_video_header}" _ps3_installed_video_sha)
        if(NOT _ps3_source_video_sha STREQUAL _ps3_installed_video_sha)
            message(FATAL_ERROR
                "ps3-self: stale sysutil/video.h in SDK install.\n"
                "  source:    ${_ps3_source_video_sha}\n"
                "  installed: ${_ps3_installed_video_sha}\n"
                "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
        endif()


        # Source-tree installs are strict: the aliases must be real symlinks
        # pointing at their target, which is what sdk/Makefile installs.  The
        # host-agnostic block above already accepted a byte-identical copy,
        # which is what a Windows package legitimately ships.
        foreach(_line IN LISTS _ps3_alias_lines)
            string(REGEX REPLACE "^alias[ 	]+" "" _row "${_line}")
            string(REGEX MATCH "^[^ 	]+" _rel "${_row}")
            string(REGEX REPLACE "^[^ 	]+[ 	]+" "" _tgt "${_row}")
            string(STRIP "${_tgt}" _tgt)
            if(NOT IS_SYMLINK "${PS3DK}/${_rel}")
                message(FATAL_ERROR
                    "ps3-self: ${PS3DK}/${_rel} must be a symlink to ${_tgt}"
                    "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
            endif()
            file(READ_SYMLINK "${PS3DK}/${_rel}" _ps3_alias_target)
            if(NOT _ps3_alias_target STREQUAL "${_tgt}")
                message(FATAL_ERROR
                    "ps3-self: ${PS3DK}/${_rel} points to ${_ps3_alias_target}, expected ${_tgt}"
                    "  Run: make -C ${_PS3_TOOLCHAIN_ROOT}/sdk install")
            endif()
        endforeach()
    endif()
endif()

# -----------------------------------------------------------------------------
# ps3_add_self(target [TITLE str] [APPID str] [CONTENTID str])
# -----------------------------------------------------------------------------
# TITLE / APPID / CONTENTID are reserved for the .pkg target which a
# subsequent phase (7c+) will wire up via make_self_npdrm + sfo +
# pkg + package_finalize.  For the MVP the function only emits
# the .self / .fake.self post-build chain.
function(ps3_add_self target)
    cmake_parse_arguments(_PSA "" "TITLE;APPID;CONTENTID" "" ${ARGN})

    if(_PSA_UNPARSED_ARGUMENTS)
        message(WARNING "ps3_add_self: unrecognised arguments ignored: ${_PSA_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_self: target '${target}' does not exist")
    endif()

    # The unstripped .elf comes from CMake's add_executable target and
    # lives in the build dir; only the post-build artefacts move out
    # to the sample source dir (next to CMakeLists.txt) so they sit
    # exactly where the legacy Makefile placed `${TARGET}.{elf,self}`.
    # CMAKE_CURRENT_SOURCE_DIR is the dir containing the calling
    # CMakeLists, which is what we want for samples invoked via
    # `cmake -S <sample-dir>`.
    set(_elf       "$<TARGET_FILE:${target}>")
    set(_stripped  "${CMAKE_CURRENT_SOURCE_DIR}/${target}.elf")
    set(_self      "${CMAKE_CURRENT_SOURCE_DIR}/${target}.self")
    set(_fake_self "${CMAKE_CURRENT_SOURCE_DIR}/${target}.fake.self")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_STRIP}" "${_elf}" -o "${_stripped}"
        COMMAND "${PS3_TOOL_sprxlinker}" ${PS3_SPRXLINKER_FLAGS} "${_stripped}"
        COMMAND "${PS3_TOOL_make_self}"  "${_stripped}" "${_self}"
        COMMAND "${PS3_TOOL_fself}"      "${_stripped}" "${_fake_self}"
        BYPRODUCTS "${_stripped}" "${_self}" "${_fake_self}"
        COMMENT "ps3-self: ${target}.{self,fake.self}"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# ps3_add_prx(target NAME name VERSION M.m [ATTRIBUTES hex] [SIGN])
# ps3_add_sprx(...) is an alias kept for callers that prefer the file suffix.
# -----------------------------------------------------------------------------
function(ps3_add_prx target)
    cmake_parse_arguments(_PSP "SIGN" "NAME;VERSION;ATTRIBUTES;EXPORTS;OUTPUT;OUTPUT_NAME" "" ${ARGN})

    if(_PSP_UNPARSED_ARGUMENTS)
        message(WARNING "ps3_add_prx: unrecognised arguments ignored: ${_PSP_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_prx: target '${target}' does not exist")
    endif()
    if(NOT _PSP_NAME)
        message(FATAL_ERROR "ps3_add_prx: NAME is required")
    endif()
    if(NOT _PSP_VERSION)
        message(FATAL_ERROR "ps3_add_prx: VERSION is required")
    endif()
    if(NOT PS3_TOOL_prx_gen)
        message(FATAL_ERROR
            "ps3_add_prx: required host tool 'prx-gen${_ps3_prx_exe}' not found.\n"
            "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.")
    endif()
    # SIGN emits BOTH containers, mirroring ps3_add_self exactly:
    #
    #   <stem>.sprx        make_sprx  -- real-signed, pairs with <target>.self
    #   <stem>.fake.sprx   fself      -- s_flags 0x8000, pairs with <target>.fake.self
    #
    # The pairing is the point. A module's container has to match the
    # executable's: ship a real .self and its modules must be real .sprx, boot a
    # .fake.self and the modules it loads must be fake too. Emitting both means
    # a project never has to choose at configure time -- it loads whichever
    # matches how it was built, exactly as it already picks .self vs .fake.self.
    #
    # Real signing is a genuine option, not a placeholder: the retail keys have
    # been public since the 2010 ECDSA nonce-reuse disclosure, which is why
    # community CFW installs through the official updater. What is unresolved is
    # narrower -- RPCS3 rejects make_sprx's current output with
    # CELL_PRX_ERROR_UNSUPPORTED_PRX_TYPE ("Failed to decrypt file") before it
    # parses the ELF, i.e. its key table has no entry matching the container's
    # (program type, s_flags, sceversion) triple. Whether real hardware accepts
    # it is untested. See t_09bf2ec9.
    if(_PSP_SIGN AND NOT PS3_TOOL_make_sprx)
        message(FATAL_ERROR
            "ps3_add_prx: SIGN requested but 'make_sprx${_ps3_prx_exe}' was not found.\n"
            "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.")
    endif()
    if(_PSP_SIGN AND NOT PS3_TOOL_fself)
        message(FATAL_ERROR
            "ps3_add_prx: SIGN requested but 'fself${_ps3_prx_exe}' was not found.\n"
            "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.")
    endif()

    foreach(_rel ppu/lib/lv2-prx.specs ppu/lib/lv2-prx.ld ppu/lib/lv2-prx-crt.o)
        if(NOT EXISTS "${PS3DK}/${_rel}")
            message(FATAL_ERROR
                "ps3_add_prx: required PRX runtime artifact missing: ${PS3DK}/${_rel}\n"
                "  Run scripts/build-runtime-lv2.sh after the PRX runtime lane lands.")
        endif()
    endforeach()

    set(_psp_attributes "${_PSP_ATTRIBUTES}")
    if(NOT _psp_attributes)
        set(_psp_attributes "0x0")
    endif()

    target_link_options(${target} PRIVATE
        "-specs=${PS3DK}/ppu/lib/lv2-prx.specs"
        "-nostartfiles"
        "-Wl,-q"
        "-Wl,--no-warn-rwx-segments"
        "-Wl,-T,${PS3DK}/ppu/lib/lv2-prx.ld")
    target_link_libraries(${target} PRIVATE "${PS3DK}/ppu/lib/lv2-prx-crt.o")

    set(_prx_output "${_PSP_OUTPUT}")
    if(NOT _prx_output)
        set(_prx_name "${_PSP_OUTPUT_NAME}")
        if(NOT _prx_name)
            set(_prx_name "${target}.prx")
        endif()
        set(_prx_output "${CMAKE_CURRENT_SOURCE_DIR}/${_prx_name}")
    endif()

    set(_byproducts "${_prx_output}")
    set(_sign_commands "")
    if(_PSP_SIGN)
        get_filename_component(_prx_dir "${_prx_output}" DIRECTORY)
        get_filename_component(_prx_stem "${_prx_output}" NAME_WE)
        set(_sprx_output      "${_prx_dir}/${_prx_stem}.sprx")
        set(_fake_sprx_output "${_prx_dir}/${_prx_stem}.fake.sprx")
        list(APPEND _byproducts "${_sprx_output}" "${_fake_sprx_output}")
        list(APPEND _sign_commands
            COMMAND "${PS3_TOOL_make_sprx}" "${_prx_output}" "${_sprx_output}"
            COMMAND "${PS3_TOOL_fself}"     "${_prx_output}" "${_fake_sprx_output}")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${PS3_TOOL_prx_gen}" build "$<TARGET_FILE:${target}>"
                -o "${_prx_output}"
                --name "${_PSP_NAME}"
                --version "${_PSP_VERSION}"
                --attributes "${_psp_attributes}"
        COMMAND "${PS3_TOOL_prx_gen}" check "${_prx_output}"
        ${_sign_commands}
        BYPRODUCTS ${_byproducts}
        COMMENT "ps3-prx: ${target} -> ${_prx_output}"
        VERBATIM)

    set_property(TARGET ${target} PROPERTY PS3_PRX_OUTPUT "${_prx_output}")
    set_property(TARGET ${target} PROPERTY PS3_PRX_EXPORTS "${_PSP_EXPORTS}")
endfunction()

# -----------------------------------------------------------------------------
# ps3_prx_export_roundtrip(<target>)
#
# Proves the export round trip on a module built by ps3_add_prx: read the
# built module's export table back out with `prx-gen exports`, and check it
# describes the same library as the YAML that produced it.
#
# The comparison is deliberately NOT a text diff of the two YAMLs. An export
# table cannot carry everything a hand-written YAML holds -- archive_name and
# the C signature have nowhere to live in a .lib.ent record -- so the files
# legitimately differ. What must match is the part that a linker consumes:
# the library name, and the export names and NIDs in order. Running both
# through `nidgen entgen` reduces each to exactly that, so a byte comparison
# of the generated assembly is the real equivalence check.
#
# Failure here means a module's own export table disagrees with the source of
# truth it was generated from -- which is precisely the bug that would ship a
# stub archive nothing can link against.
# -----------------------------------------------------------------------------
function(ps3_prx_export_roundtrip target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_prx_export_roundtrip: target '${target}' does not exist")
    endif()
    if(NOT PS3_TOOL_nidgen)
        message(FATAL_ERROR
            "ps3_prx_export_roundtrip: required host tool 'nidgen${_ps3_prx_exe}' not found.\n"
            "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.")
    endif()

    get_property(_rt_prx TARGET ${target} PROPERTY PS3_PRX_OUTPUT)
    get_property(_rt_yml TARGET ${target} PROPERTY PS3_PRX_EXPORTS)
    if(NOT _rt_prx OR NOT _rt_yml)
        message(FATAL_ERROR
            "ps3_prx_export_roundtrip: call ps3_add_prx(${target} ... EXPORTS <yml>) first")
    endif()
    if(NOT IS_ABSOLUTE "${_rt_yml}")
        set(_rt_yml "${CMAKE_CURRENT_SOURCE_DIR}/${_rt_yml}")
    endif()

    set(_rt_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}-roundtrip")
    set(_rt_stamp "${_rt_dir}/roundtrip.stamp")

    add_custom_command(
        OUTPUT "${_rt_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_rt_dir}"
        COMMAND "${PS3_TOOL_prx_gen}" exports "${_rt_prx}" -o "${_rt_dir}/extracted.yml"
        COMMAND "${PS3_TOOL_nidgen}" entgen --input "${_rt_yml}"
                -o "${_rt_dir}/from-source.S"
        COMMAND "${PS3_TOOL_nidgen}" entgen --input "${_rt_dir}/extracted.yml"
                -o "${_rt_dir}/from-module.S"
        COMMAND "${CMAKE_COMMAND}"
                "-DPS3_RT_SOURCE=${_rt_dir}/from-source.S"
                "-DPS3_RT_MODULE=${_rt_dir}/from-module.S"
                "-DPS3_RT_TARGET=${target}"
                -P "${_PS3_SELF_CMAKE_DIR}/ps3-prx-roundtrip-check.cmake"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_rt_stamp}"
        DEPENDS ${target} "${_rt_yml}"
        COMMENT "ps3-prx: export round trip for ${target}"
        VERBATIM)

    add_custom_target(${target}_export_roundtrip ALL DEPENDS "${_rt_stamp}")
endfunction()

function(ps3_add_sprx target)
    ps3_add_prx(${target} ${ARGN})
endfunction()

# -----------------------------------------------------------------------------
# ps3_prx_stub_library(target EXPORTS exports.yml [ARCHIVE_NAME name] [ABI mode])
# -----------------------------------------------------------------------------
function(ps3_prx_stub_library target)
    cmake_parse_arguments(_PSS "" "EXPORTS;ARCHIVE_NAME;ABI" "" ${ARGN})

    if(_PSS_UNPARSED_ARGUMENTS)
        message(WARNING "ps3_prx_stub_library: unrecognised arguments ignored: ${_PSS_UNPARSED_ARGUMENTS}")
    endif()
    if(TARGET ${target})
        message(FATAL_ERROR "ps3_prx_stub_library: target '${target}' already exists")
    endif()
    if(NOT _PSS_EXPORTS)
        message(FATAL_ERROR "ps3_prx_stub_library: EXPORTS is required")
    endif()
    if(NOT PS3_TOOL_nidgen)
        message(FATAL_ERROR
            "ps3_prx_stub_library: required host tool 'nidgen${_ps3_prx_exe}' not found.\n"
            "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.")
    endif()

    if(IS_ABSOLUTE "${_PSS_EXPORTS}")
        set(_exports "${_PSS_EXPORTS}")
    else()
        set(_exports "${CMAKE_CURRENT_SOURCE_DIR}/${_PSS_EXPORTS}")
    endif()
    if(NOT EXISTS "${_exports}")
        message(FATAL_ERROR "ps3_prx_stub_library: exports YAML not found: ${_exports}")
    endif()

    set(_archive_name "${_PSS_ARCHIVE_NAME}")
    if(NOT _archive_name)
        set(_archive_name "${target}")
        string(REGEX REPLACE "_stub$" "" _archive_name "${_archive_name}")
    endif()

    set(_abi "${_PSS_ABI}")
    if(NOT _abi)
        set(_abi "ilp32")
    endif()

    get_filename_component(_ppu_tool_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    set(_tool_suffix "")
    if(CMAKE_HOST_WIN32)
        set(_tool_suffix ".exe")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(_archive "${_out_dir}/lib${_archive_name}_stub.a")
    add_custom_command(
        OUTPUT "${_archive}"
        COMMAND "${PS3_TOOL_nidgen}" archive
                --input "${_exports}"
                --toolchain-bin "${_ppu_tool_bin}"
                --asm "powerpc64-ps3-elf-as${_tool_suffix}"
                --ar "powerpc64-ps3-elf-ar${_tool_suffix}"
                --out-dir "${_out_dir}"
                --abi "${_abi}"
        DEPENDS "${_exports}"
        COMMENT "ps3-prx: ${target} import stub"
        VERBATIM)
    add_custom_target(${target}_archive DEPENDS "${_archive}")

    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_archive}")
    add_dependencies(${target} ${target}_archive)
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
    find_program(PS3_SPU_OBJCOPY
        NAMES "spu-elf-objcopy${_ps3_self_exe}" "spu-elf-objcopy"
        PATHS "${PS3DEV}/spu/bin"
        NO_DEFAULT_PATH)
    find_program(PS3_TOOL_spu_elf_to_ppu_obj
        NAMES "spu-elf-to-ppu-obj${_ps3_self_exe}" "spu-elf-to-ppu-obj"
        PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
              "${CMAKE_CURRENT_LIST_DIR}/../tools/target/release"
              "${CMAKE_CURRENT_LIST_DIR}/../tools/target/debug"
        NO_DEFAULT_PATH)
    if(NOT PS3_TOOL_spu_elf_to_ppu_obj)
        find_program(PS3_TOOL_spu_elf_to_ppu_obj
            NAMES "spu-elf-to-ppu-obj${_ps3_self_exe}" "spu-elf-to-ppu-obj")
    endif()
    # Optional reference-helper path for black-box diff/debug only.
    # Normal JOBBIN_WRAP builds use the independent spu-elf-to-ppu-obj tool.
    set(_ps3_ref_spu_elf_to_ppu_obj "$ENV{PS3_SPU_ELF_TO_PPU_OBJ}")
    if(_ps3_ref_spu_elf_to_ppu_obj)
        set(PS3_TOOL_ref_spu_elf_to_ppu_obj "${_ps3_ref_spu_elf_to_ppu_obj}" CACHE FILEPATH
            "Optional reference SDK spu_elf-to-ppu_obj.exe for JOBBIN_WRAP diff/debug")
    endif()
    set(_ps3_jobbin2_use_reference_helper FALSE)
    if(PS3_TOOL_ref_spu_elf_to_ppu_obj)
        set(_ps3_jobbin2_use_reference_helper TRUE)
    endif()
    if(NOT CMAKE_HOST_WIN32 AND PS3_TOOL_ref_spu_elf_to_ppu_obj)
        find_program(PS3_TOOL_wine NAMES "wine")
    endif()
    set(_ps3_jobbin2_wine_path "$ENV{PS3_PPU_LV2_GCC_BIN_DIR}")
    set(PS3_TOOL_ppu_lv2_gcc_dir "${_ps3_jobbin2_wine_path}" CACHE PATH
        "Directory containing reference ppu-lv2-gcc.exe for wine PATH")
    # Optional — only samples that embed SPU code need the SPU compiler.
endif()

function(ps3_add_spu_image target)
    cmake_parse_arguments(_PSI
        "NOSTARTFILES;FREESTANDING;JOBBIN;JOBBIN_WRAP"  # boolean flags
        "NAME;LDSCRIPT"                                 # single-value
        "SOURCES;LIBS;CFLAGS;LDFLAGS"                   # multi-value
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
    # Wrap multi-lib lists in --start-group/--end-group so the linker
    # re-scans for cross-archive symbols (e.g. libspurs_job's _start
    # references cellSpursJobMain2 which lives in libspurs_jq).
    # Single-lib lists go through unwrapped - --start-group breaks
    # JOB CHAIN's hello-spu-job for unknown reasons.
    set(_spu_libs)
    list(LENGTH _PSI_LIBS _spu_libs_count)
    if(_spu_libs_count GREATER 1)
        list(APPEND _spu_libs "-Wl,--start-group")
        foreach(lib ${_PSI_LIBS})
            list(APPEND _spu_libs "-l${lib}")
        endforeach()
        list(APPEND _spu_libs "-Wl,--end-group")
    else()
        foreach(lib ${_PSI_LIBS})
            list(APPEND _spu_libs "-l${lib}")
        endforeach()
    endif()

    add_custom_command(
        OUTPUT "${_spu_elf}"
        COMMAND "${PS3_SPU_GCC}"
                ${_spu_link_flags}
                ${_spu_objs} ${_spu_libs}
                -o "${_spu_elf}"
        DEPENDS ${_link_deps}
        COMMENT "ps3-spu: link ${_PSI_NAME}.elf"
        VERBATIM)

    # JOBBIN: the SPRX JOB-CHAIN dispatcher DMAs raw bytes from
    # descriptor.eaBinary straight into LS — no ELF wrapper.  Convert the
    # linked SPU ELF to a flat binary image via objcopy -O binary so
    # eaBinary points at the LS-image start (.SpuGUID magic at offset 0).
    #
    # JOBBIN_WRAP: SPURS JOB-QUEUE workloads need jobbin2 wrapper bytes:
    # a 0x100-byte ELF32/SPU prefix, a patched LS image at blob+0x100,
    # and a byte-correct CellSpursJobHeader template.  The independent
    # spu-elf-to-ppu-obj host tool emits a .ppu.o with .spu_image and
    # .spu_image.jobheader plus transition sidecars that the current
    # bin2s embedding path consumes.
    if(_PSI_JOBBIN_WRAP)
        if(NOT PS3_TOOL_spu_elf_to_ppu_obj AND NOT _ps3_jobbin2_use_reference_helper)
            message(FATAL_ERROR "ps3_add_spu_image: JOBBIN_WRAP needs spu-elf-to-ppu-obj (install it under ${PS3DEV}/bin, ${PS3DK}/bin, or put it on PATH)")
        endif()
        if(NOT CMAKE_HOST_WIN32 AND _ps3_jobbin2_use_reference_helper AND NOT PS3_TOOL_wine)
            message(FATAL_ERROR "ps3_add_spu_image: reference-helper JOBBIN_WRAP requires wine on PATH")
        endif()
        if(NOT CMAKE_HOST_WIN32 AND _ps3_jobbin2_use_reference_helper AND NOT PS3_TOOL_ppu_lv2_gcc_dir)
            message(FATAL_ERROR "ps3_add_spu_image: reference-helper JOBBIN_WRAP needs the reference ppu-lv2-gcc.exe dir on wine PATH (set PS3_PPU_LV2_GCC_BIN_DIR env)")
        endif()
        find_program(PS3_PPU_OBJCOPY
            NAMES "${PS3_PPU_TARGET}-objcopy${_ps3_self_exe}"
                  "powerpc64-ps3-elf-objcopy${_ps3_self_exe}"
                  "powerpc64-ps3-elf-objcopy"
            PATHS "${PS3DEV}/ppu/bin"
            NO_DEFAULT_PATH)
        if(NOT PS3_PPU_OBJCOPY)
            message(FATAL_ERROR "ps3_add_spu_image: JOBBIN_WRAP requires powerpc64-ps3-elf-objcopy")
        endif()
        set(_spu_jobbin_dir "${_spu_dir}/jobbin")
        file(MAKE_DIRECTORY "${_spu_jobbin_dir}")
        set(_spu_jobbin_ppu_o   "${_spu_jobbin_dir}/${_PSI_NAME}.ppu.o")
        set(_spu_jobbin_blob    "${_spu_jobbin_dir}/${_PSI_NAME}.jobbin2")
        set(_spu_jobbin_bin     "${_spu_jobbin_dir}/${_PSI_NAME}.bin")
        set(_spu_jobheader_bin  "${_spu_jobbin_dir}/${_PSI_NAME}_jobheader.bin")
        if(NOT _ps3_jobbin2_use_reference_helper AND PS3_TOOL_spu_elf_to_ppu_obj)
            add_custom_command(
                OUTPUT "${_spu_jobbin_ppu_o}" "${_spu_jobbin_blob}" "${_spu_jobbin_bin}" "${_spu_jobheader_bin}"
                COMMAND "${PS3_TOOL_spu_elf_to_ppu_obj}"
                        wrap
                        --spu-elf "${_spu_elf}"
                        --output "${_spu_jobbin_ppu_o}"
                        --symbol-base "${_PSI_NAME}"
                        --emit-sidecars
                COMMAND ${CMAKE_COMMAND} -E copy "${_spu_jobbin_blob}" "${_spu_jobbin_bin}"
                DEPENDS "${_spu_elf}"
                COMMENT "ps3-spu: spu-elf-to-ppu-obj ${_PSI_NAME}.jobbin2"
                VERBATIM)
        elseif(CMAKE_HOST_WIN32)
            add_custom_command(
                OUTPUT "${_spu_jobbin_ppu_o}" "${_spu_jobbin_blob}" "${_spu_jobbin_bin}" "${_spu_jobheader_bin}"
                COMMAND ${CMAKE_COMMAND} -E copy "${_spu_elf}" "${_spu_jobbin_dir}/${_PSI_NAME}.elf"
                COMMAND "${PS3_TOOL_ref_spu_elf_to_ppu_obj}"
                        --format=jobbin2
                        --objcopy-style-symbol
                        "${_spu_jobbin_dir}/${_PSI_NAME}.elf"
                        "${_spu_jobbin_ppu_o}"
                COMMAND ${CMAKE_COMMAND} -E copy "${_spu_jobbin_blob}" "${_spu_jobbin_bin}"
                COMMAND "${PS3_PPU_OBJCOPY}" --output-target=binary
                        --only-section=.spu_image.jobheader
                        "${_spu_jobbin_ppu_o}" "${_spu_jobheader_bin}"
                DEPENDS "${_spu_elf}"
                COMMENT "ps3-spu: spu-elf-to-ppu-obj ${_PSI_NAME}.jobbin2 (reference helper)"
                VERBATIM)
        else()
            add_custom_command(
                OUTPUT "${_spu_jobbin_ppu_o}" "${_spu_jobbin_blob}" "${_spu_jobbin_bin}" "${_spu_jobheader_bin}"
                COMMAND ${CMAKE_COMMAND} -E copy "${_spu_elf}" "${_spu_jobbin_dir}/${_PSI_NAME}.elf"
                COMMAND ${CMAKE_COMMAND} -E env
                        "WINEPATH=Z:${PS3_TOOL_ppu_lv2_gcc_dir}"
                        "${PS3_TOOL_wine}"
                        "${PS3_TOOL_ref_spu_elf_to_ppu_obj}"
                        --format=jobbin2
                        --objcopy-style-symbol
                        "${_spu_jobbin_dir}/${_PSI_NAME}.elf"
                        "${_spu_jobbin_ppu_o}"
                COMMAND ${CMAKE_COMMAND} -E copy "${_spu_jobbin_blob}" "${_spu_jobbin_bin}"
                COMMAND "${PS3_PPU_OBJCOPY}" --output-target=binary
                        --only-section=.spu_image.jobheader
                        "${_spu_jobbin_ppu_o}" "${_spu_jobheader_bin}"
                DEPENDS "${_spu_elf}"
                COMMENT "ps3-spu: spu-elf-to-ppu-obj ${_PSI_NAME}.jobbin2 (wine reference helper)"
                VERBATIM)
        endif()
        ps3_bin2s(${target} "${_spu_jobbin_bin}")
        ps3_bin2s(${target} "${_spu_jobheader_bin}")
    elseif(_PSI_JOBBIN)
        if(NOT PS3_SPU_OBJCOPY)
            message(FATAL_ERROR "ps3_add_spu_image: JOBBIN requires spu-elf-objcopy at ${PS3DEV}/spu/bin")
        endif()
        set(_spu_jobbin_dir "${_spu_dir}/jobbin")
        file(MAKE_DIRECTORY "${_spu_jobbin_dir}")
        set(_spu_jobbin "${_spu_jobbin_dir}/${_PSI_NAME}.bin")
        add_custom_command(
            OUTPUT "${_spu_jobbin}"
            COMMAND "${PS3_SPU_OBJCOPY}" -O binary "${_spu_elf}" "${_spu_jobbin}"
            DEPENDS "${_spu_elf}"
            COMMENT "ps3-spu: objcopy ${_PSI_NAME}.bin (flat job image)"
            VERBATIM)
        ps3_bin2s(${target} "${_spu_jobbin}")
    else()
        # Embed the SPU ELF into the PPU target via bin2s.  Symbol
        # prefix derives from the basename: "<NAME>.bin" → "<NAME>_bin".
        ps3_bin2s(${target} "${_spu_elf}")
    endif()
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


# ps3_add_cg_shader_rsxcgc(<target> <file>)
#
# Same as ps3_add_cg_shader but compiles through rsx-cg-compiler instead
# of cgcomp.  Use this when the consumer code calls into the cellGcmCg*
# program-handle API (cellGcmCgInitProgram, cellGcmCgGetUCode,
# cellGcmCgGetNamedParameter, cellGcmSetVertexProgram, ...).
#
# Why a separate function instead of a flag on the cgcomp variant:
# rsx-cg-compiler emits CgBinaryProgram blobs (the reference compiler-compatible —
# magic 0x00001b5b for VP, 0x00001b5c for FP).  cgcomp emits a
# different layout starting with "VP\0\0".  The cellGcmCg* helpers in
# libgcm_cmd.a walk the CgBinaryProgram layout directly; passing them
# a cgcomp blob crashes with a VM access violation in cellGcmCgGetUCode
# (the helper reads `prog->ucode` at +28, which lands on a cgcomp
# header byte that's not a valid offset).
#
# rsxLoadVertexProgram / rsxLoadFragmentProgramLocation (PSL1GHT) only
# understand the cgcomp layout, so samples that use the rsxLoad* path
# must keep using ps3_add_cg_shader.
if(NOT _PS3_RSXCGC_PROBED)
    set(_PS3_RSXCGC_PROBED TRUE)
    set(_ps3_rsxcgc_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_rsxcgc_exe ".exe")
    endif()
    find_program(PS3_TOOL_rsxcgc
        NAMES "rsx-cg-compiler${_ps3_rsxcgc_exe}" "rsx-cg-compiler"
        PATHS
            "${PS3DEV}/bin"
            "${PS3DK}/bin"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/rsx-cg-compiler/build"
        NO_DEFAULT_PATH)
endif()

function(ps3_add_cg_shader_rsxcgc target file)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_cg_shader_rsxcgc: target '${target}' does not exist")
    endif()
    if(NOT PS3_TOOL_rsxcgc)
        message(FATAL_ERROR "ps3_add_cg_shader_rsxcgc: rsx-cg-compiler not found "
                            "(checked ${PS3DEV}/bin, ${PS3DK}/bin, tools/rsx-cg-compiler/build)")
    endif()

    if(IS_ABSOLUTE "${file}")
        set(_input "${file}")
    else()
        set(_input "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
    endif()
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "ps3_add_cg_shader_rsxcgc: input file does not exist: ${_input}")
    endif()

    get_filename_component(_ext "${_input}" EXT)
    get_filename_component(_stem "${_input}" NAME_WE)
    if(_ext STREQUAL ".vcg")
        set(_profile_arg "sce_vp_rsx")
        set(_out_ext "vpo")
    elseif(_ext STREQUAL ".fcg")
        set(_profile_arg "sce_fp_rsx")
        set(_out_ext "fpo")
    else()
        message(FATAL_ERROR "ps3_add_cg_shader_rsxcgc: ${file} has unrecognised extension "
                            "${_ext} (expected .vcg or .fcg)")
    endif()

    set(_outdir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${_outdir}")
    set(_compiled "${_outdir}/${_stem}.${_out_ext}")

    add_custom_command(
        OUTPUT  "${_compiled}"
        COMMAND "${PS3_TOOL_rsxcgc}" "-p" "${_profile_arg}"
                                      "--emit-container" "${_compiled}" "${_input}"
        DEPENDS "${_input}"
        COMMENT "ps3-rsxcgc: ${_stem}${_ext} → ${_stem}.${_out_ext}"
        VERBATIM)

    ps3_bin2s(${target} "${_compiled}")
endfunction()


# ps3_add_pkg(<target> CONTENTID str
#                       [TITLE str] [APPID str]
#                       [ICON path] [SFOXML path] [PKGFILES dir])
#
# Builds an installable PS3 .pkg (the artifact a PS3 sees on a memory
# stick / USB / a .pkg drag onto RPCS3) for `target`, which must
# already have ps3_add_self() applied.
#
# Pipeline (matches the PSL1GHT ppu_rules .pkg recipe):
#
#   1. make_self_npdrm <stripped.elf> <pkg/USRDIR/EBOOT.BIN> <CONTENTID>
#   2. sfo --title "..." --appid "..." -f <SFOXML> <pkg/PARAM.SFO>
#   3. cp <ICON> <pkg/ICON0.PNG>
#   4. cp -r <PKGFILES>/* <pkg/>     (if PKGFILES dir exists)
#   5. pkg --contentid <CONTENTID> <pkg/> <target>.pkg
#   6. package_finalize <target>.gnpdrm.pkg
#
# CONTENTID is required (36 chars: "XX0000-AAAAAAAAA_00-USERNAMEXXXXXX0").
# RPCS3 namespaces its shader cache + per-game data by the title-id
# portion (chars 8..16 of the contentid), which is the reason this
# helper exists — installing as a .pkg lets RPCS3 cache shaders against
# a stable id rather than the .self path.
#
# TITLE defaults to the target name; APPID defaults to the title-id
# extracted from the contentid; ICON defaults to sdk/assets/ICON0.PNG;
# SFOXML defaults to cmake/templates/sfo.xml; PKGFILES defaults to
# the sample's pkg_files/ subdir if it exists.
if(NOT _PS3_PKG_PROBED)
    set(_PS3_PKG_PROBED TRUE)
    set(_ps3_self_exe "")
    if(CMAKE_HOST_WIN32)
        set(_ps3_self_exe ".exe")
    endif()
    foreach(tool make_self_npdrm pkg sfo package_finalize)
        find_program(PS3_TOOL_${tool}
            NAMES "${tool}${_ps3_self_exe}" "${tool}"
            PATHS "${PS3DEV}/bin" "${PS3DK}/bin"
            NO_DEFAULT_PATH)
    endforeach()
endif()

function(ps3_add_pkg target)
    cmake_parse_arguments(_PSP "" "TITLE;APPID;CONTENTID;ICON;SFOXML;PKGFILES" "" ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR "ps3_add_pkg: target '${target}' does not exist")
    endif()
    foreach(tool make_self_npdrm pkg sfo package_finalize)
        if(NOT PS3_TOOL_${tool})
            message(FATAL_ERROR
                "ps3_add_pkg: required host tool '${tool}${_ps3_self_exe}' not found.\n"
                "  Searched: ${PS3DEV}/bin and ${PS3DK}/bin.\n"
                "  sfo/pkg are native tools as of this SDK; an install that only has\n"
                "  sfo.py/pkg.py predates them. Re-extract the release zip, or re-run\n"
                "  scripts/build-host-tools-windows.sh and scripts/install-host-tools.sh.")
        endif()
    endforeach()

    if(NOT _PSP_CONTENTID)
        message(FATAL_ERROR "ps3_add_pkg(${target}): CONTENTID is required (36-char "
                            "XX0000-AAAAAAAAA_00-USERNAMEXXXXXX0 string)")
    endif()
    string(LENGTH "${_PSP_CONTENTID}" _cid_len)
    if(NOT _cid_len EQUAL 36)
        message(WARNING "ps3_add_pkg(${target}): CONTENTID is ${_cid_len} chars, "
                        "expected 36.  Real PS3 hardware will reject the .pkg.")
    endif()

    if(NOT _PSP_TITLE)
        set(_PSP_TITLE "${target}")
    endif()
    if(NOT _PSP_APPID)
        # Extract the 9-char title-id from the contentid (chars 8..16).
        string(SUBSTRING "${_PSP_CONTENTID}" 7 9 _PSP_APPID)
    endif()

    # Default icon: the toolchain's branded sdk/assets/ICON0.PNG.
    if(NOT _PSP_ICON)
        set(_PSP_ICON "${_PS3_TOOLCHAIN_ROOT}/sdk/assets/ICON0.PNG")
    endif()
    if(NOT EXISTS "${_PSP_ICON}")
        message(FATAL_ERROR "ps3_add_pkg(${target}): ICON not found: ${_PSP_ICON}")
    endif()

    # Default SFO XML — PSL1GHT-style template; TITLE/APPID overridable
    # from the command line via sfo's --title / --appid flags.
    if(NOT _PSP_SFOXML)
        set(_PSP_SFOXML "${_PS3_SELF_CMAKE_DIR}/templates/sfo.xml")
    endif()
    if(NOT EXISTS "${_PSP_SFOXML}")
        message(FATAL_ERROR "ps3_add_pkg(${target}): SFOXML not found: ${_PSP_SFOXML}")
    endif()

    # The .self post-build chain lands a stripped .elf at
    # <src>/<target>.elf — that's what make_self_npdrm signs.
    set(_stripped  "${CMAKE_CURRENT_SOURCE_DIR}/${target}.elf")
    set(_pkg_dir   "${CMAKE_CURRENT_BINARY_DIR}/pkg")
    set(_pkg_out   "${CMAKE_CURRENT_SOURCE_DIR}/${target}.pkg")
    set(_pkg_npdrm "${CMAKE_CURRENT_SOURCE_DIR}/${target}.gnpdrm.pkg")

    # Optional pkg_files/ overlay (e.g. hello-ppu-png uses this).
    if(NOT _PSP_PKGFILES)
        set(_PSP_PKGFILES "${CMAKE_CURRENT_SOURCE_DIR}/pkg_files")
    endif()
    if(EXISTS "${_PSP_PKGFILES}")
        set(_pkg_overlay_cmd
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_PSP_PKGFILES}" "${_pkg_dir}")
    else()
        set(_pkg_overlay_cmd "")
    endif()

    add_custom_command(
        OUTPUT "${_pkg_out}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_pkg_dir}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_pkg_dir}/USRDIR"
        COMMAND "${PS3_TOOL_make_self_npdrm}" "${_stripped}"
                "${_pkg_dir}/USRDIR/EBOOT.BIN" "${_PSP_CONTENTID}"
        COMMAND "${PS3_TOOL_sfo}"
                --title "${_PSP_TITLE}" --appid "${_PSP_APPID}"
                -f "${_PSP_SFOXML}" "${_pkg_dir}/PARAM.SFO"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_PSP_ICON}" "${_pkg_dir}/ICON0.PNG"
        ${_pkg_overlay_cmd}
        COMMAND "${PS3_TOOL_pkg}"
                --contentid "${_PSP_CONTENTID}" "${_pkg_dir}/" "${_pkg_out}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_pkg_out}" "${_pkg_npdrm}"
        COMMAND "${PS3_TOOL_package_finalize}" "${_pkg_npdrm}"
        DEPENDS "${_stripped}" "${_PSP_ICON}" "${_PSP_SFOXML}"
        BYPRODUCTS "${_pkg_dir}" "${_pkg_npdrm}"
        COMMENT "ps3-pkg: ${target}.pkg (CONTENTID=${_PSP_CONTENTID})"
        VERBATIM)

    add_custom_target(${target}_pkg ALL DEPENDS "${_pkg_out}")
    add_dependencies(${target}_pkg ${target})
endfunction()

cmake_minimum_required(VERSION 3.20)
project(miniaudio_thread_cycle_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Host cannot validate Lv-2 pthreads" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()

include(ps3-self)

# A real third-party pthread consumer (t_e87d59d7): miniaudio 0.11.25 as released, its null backend driving a device
# worker thread created and joined through our librt pthreads.  The header is fetched at configure time from the pinned
# upstream commit and checked against its SHA-256 (the upstream LF bytes); a download or hash failure stops the
# configure, so this row can never be skipped silently.  PS3TC_MINIAUDIO_H points at a pre-fetched copy (offline
# builds); it is hash-checked the same way.
set(MINIAUDIO_COMMIT 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d)
set(MINIAUDIO_SHA256 ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89)
set(_ma_dir "${CMAKE_CURRENT_BINARY_DIR}/miniaudio-${MINIAUDIO_COMMIT}")
set(_ma_h "${_ma_dir}/miniaudio.h")
if(DEFINED ENV{PS3TC_MINIAUDIO_H} AND NOT EXISTS "${_ma_h}")
    file(MAKE_DIRECTORY "${_ma_dir}")
    file(COPY_FILE "$ENV{PS3TC_MINIAUDIO_H}" "${_ma_h}")
endif()
if(NOT EXISTS "${_ma_h}")
    file(DOWNLOAD "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_COMMIT}/miniaudio.h" "${_ma_h}"
         EXPECTED_HASH SHA256=${MINIAUDIO_SHA256} TLS_VERIFY ON STATUS _ma_status)
    list(GET _ma_status 0 _ma_code)
    if(NOT _ma_code EQUAL 0)
        file(REMOVE "${_ma_h}")
        message(FATAL_ERROR "miniaudio-thread-cycle: cannot fetch miniaudio.h at ${MINIAUDIO_COMMIT}: ${_ma_status} "
                            "(set PS3TC_MINIAUDIO_H to a pre-fetched copy for offline builds)")
    endif()
endif()
file(SHA256 "${_ma_h}" _ma_got)
if(NOT _ma_got STREQUAL MINIAUDIO_SHA256)
    message(FATAL_ERROR "miniaudio-thread-cycle: miniaudio.h sha256 ${_ma_got}, want ${MINIAUDIO_SHA256}")
endif()

# One probe, both ABIs, as pthread-audio-path.  Our own sources keep -Werror=implicit-function-declaration; the
# miniaudio translation unit (probe.c) is compiled as upstream ships it, and every warning it produces is kept in the
# build log, not silenced.
foreach(abi ilp32 lp64)
    add_executable(miniaudio-thread-cycle-${abi} main.c probe.c)
    target_compile_features(miniaudio-thread-cycle-${abi} PRIVATE c_std_11)
    target_include_directories(miniaudio-thread-cycle-${abi} PRIVATE "${_ma_dir}")
    target_compile_options(miniaudio-thread-cycle-${abi} PRIVATE -Wall)
    set_source_files_properties(main.c PROPERTIES COMPILE_OPTIONS "-Wextra;-Werror=implicit-function-declaration")
    if(abi STREQUAL "lp64")
        target_compile_options(miniaudio-thread-cycle-${abi} PRIVATE -mlp64)
        target_link_options(miniaudio-thread-cycle-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(miniaudio-thread-cycle-${abi})
endforeach()

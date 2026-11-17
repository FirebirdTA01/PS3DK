cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-call-return C)
include(ps3-self)
set(GCM_SEG_RING_FRAME_COUNT 30 CACHE STRING "Bounded frame count for the ring retention probe (1..300)")
if(NOT GCM_SEG_RING_FRAME_COUNT MATCHES "^[0-9]+$" OR
   GCM_SEG_RING_FRAME_COUNT LESS 1 OR GCM_SEG_RING_FRAME_COUNT GREATER 300)
    message(FATAL_ERROR "GCM_SEG_RING_FRAME_COUNT must be between 1 and 300")
endif()

# One SELF per variant.  The default runs only main-memory segments (plus the
# overflow refusal, which issues no CALL); -local checks that a segment in
# RSX local memory is refused; -large runs one 1 MiB segment on its own;
# -jump chains two sub-segments with a JUMP.
foreach(variant "" "-local" "-large" "-jump" "-ring")
    set(t hello-ppu-gcm-call-return${variant})
    add_executable(${t} source/main.c)
    target_compile_features(${t} PRIVATE c_std_11)
    target_compile_options(${t} PRIVATE -O2 -Wall -Wextra -Werror)
    if(variant STREQUAL "-local")
        target_compile_definitions(${t} PRIVATE GCM_SEG_VARIANT_LOCAL=1)
    elseif(variant STREQUAL "-large")
        target_compile_definitions(${t} PRIVATE GCM_SEG_VARIANT_LARGE=1)
    elseif(variant STREQUAL "-jump")
        target_compile_definitions(${t} PRIVATE GCM_SEG_VARIANT_JUMP=1)
    elseif(variant STREQUAL "-ring")
        # owner-buffer clears above the local report ceiling, for the AUTO report ring (inert unless profiled)
        target_compile_definitions(${t} PRIVATE GCM_SEG_VARIANT_RING=1
            GCM_SEG_RING_FRAME_COUNT=${GCM_SEG_RING_FRAME_COUNT})
    else()
        set(id GCMS00001)
    endif()
    target_link_libraries(${t} PRIVATE gcm_cmd gcm_sys sysutil_stub sysutil rt lv2)
    ps3_add_self(${t})
    # ps3_add_pkg makes one project-wide "pkg" target: only the default
    # variant is packaged; the others are booted as SELFs.
    if(variant STREQUAL "")
        ps3_add_pkg(${t} APPID "${id}" CONTENTID "UP0001-${id}_00-PS3DKSAMPLES0000")
    endif()
endforeach()

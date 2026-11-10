cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-call-return C)
include(ps3-self)

# One SELF per variant.  The default runs only main-memory segments (plus the
# overflow refusal, which issues no CALL); -local and -large run the riskier
# phases on their own; -jump chains two sub-segments with a JUMP.
foreach(variant "" "-local" "-large" "-jump")
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

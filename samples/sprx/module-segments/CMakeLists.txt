cmake_minimum_required(VERSION 3.20)
project(sample-sprx-module-segments C ASM)

include(ps3-self)

# In-tree builds pick up the repository's SDK headers, so a header change
# (here <sys/prx_module.h>, whose export record shrank to 0x1c) is used even
# against an installed SDK that predates it.
get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(IS_DIRECTORY "${_repo_root}/sdk/include")
    include_directories(SYSTEM "${_repo_root}/sdk/include")
endif()

# One module + one loader per ABI. Each loader looks for its own module stem
# next to itself and in /app_home, as .sprx first and then .fake.sprx.
#
#   ILP32: sprx_segments.{prx,sprx,fake.sprx}       sprx-segments-loader.{self,fake.self}
#   LP64:  sprx_segments_lp64.{prx,sprx,fake.sprx}  sprx-segments-loader-lp64.{self,fake.self}
foreach(_abi ilp32 lp64)
    if(_abi STREQUAL "lp64")
        set(_sfx "-lp64")
        set(_stem "sprx_segments_lp64")
    else()
        set(_sfx "")
        set(_stem "sprx_segments")
    endif()

    # --- the module: two libraries + the crt's management record ---------
    set(_mod sprx-segments-module${_sfx})
    add_executable(${_mod} module/module.c module/exports.S)
    target_compile_features(${_mod} PRIVATE c_std_11)
    target_include_directories(${_mod} PRIVATE include)
    if(_abi STREQUAL "lp64")
        target_compile_options(${_mod} PRIVATE -mlp64)
        target_link_options(${_mod} PRIVATE -mlp64)
    endif()
    ps3_add_prx(${_mod}
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.prx"
        SIGN)

    # --- import stubs for both libraries ---------------------------------
    ps3_prx_stub_library(seg_calc_stub${_sfx} EXPORTS seg_calc.yml ARCHIVE_NAME seg_calc ABI ${_abi})
    ps3_prx_stub_library(seg_state_stub${_sfx} EXPORTS seg_state.yml ARCHIVE_NAME seg_state ABI ${_abi})

    # --- the loader ------------------------------------------------------
    set(_ldr sprx-segments-loader${_sfx})
    add_executable(${_ldr} loader/main.c)
    target_compile_features(${_ldr} PRIVATE c_std_11)
    target_include_directories(${_ldr} PRIVATE include)
    target_compile_definitions(${_ldr} PRIVATE MODULE_STEM="${_stem}")
    if(_abi STREQUAL "lp64")
        target_compile_options(${_ldr} PRIVATE -mlp64)
        target_link_options(${_ldr} PRIVATE -mlp64)
    endif()
    target_link_libraries(${_ldr} PRIVATE seg_calc_stub${_sfx} seg_state_stub${_sfx} rt lv2)
    ps3_add_self(${_ldr} OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

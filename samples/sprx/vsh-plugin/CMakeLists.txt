cmake_minimum_required(VERSION 3.20)
project(sample-sprx-vsh-plugin C)

include(ps3-self)

# In-tree builds pick up the repository's SDK headers, so a header change is
# used even against an installed SDK that predates it.
get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(IS_DIRECTORY "${_repo_root}/sdk/include")
    include_directories(SYSTEM "${_repo_root}/sdk/include")
endif()

# One plugin + one loader per ABI.  The plugin exports no library: a VSH
# plugin is reached only through module_start / module_stop.  Each loader
# looks for its own plugin stem next to itself and in /app_home, as .sprx,
# then .fake.sprx, then .prx.
#
#   ILP32: vsh_plugin.{prx,sprx,fake.sprx}       vsh-plugin-loader.{elf,self,fake.self}
#   LP64:  vsh_plugin_lp64.{prx,sprx,fake.sprx}  vsh-plugin-loader-lp64.{elf,self,fake.self}
#
# The ILP32 vsh_plugin.sprx is the one to load into VSH.
foreach(_abi ilp32 lp64)
    if(_abi STREQUAL "lp64")
        set(_sfx "-lp64")
        set(_stem "vsh_plugin_lp64")
    else()
        set(_sfx "")
        set(_stem "vsh_plugin")
    endif()

    # --- the plugin ---------------------------------------------------------
    set(_mod vsh-plugin-module${_sfx})
    add_executable(${_mod} module/plugin.c)
    target_compile_features(${_mod} PRIVATE c_std_11)
    target_include_directories(${_mod} PRIVATE include)
    # Under LP64, GCC places a switch's jump table in .text and reaches it
    # through r2, which crosses from the data segment (the TOC) to the code
    # segment; prx-gen refuses that.  The plugin has no switch, and this
    # keeps it that way.
    target_compile_options(${_mod} PRIVATE -Wall -Wextra -fno-jump-tables)
    if(_abi STREQUAL "lp64")
        target_compile_options(${_mod} PRIVATE -mlp64)
        target_link_options(${_mod} PRIVATE -mlp64)
    endif()
    ps3_add_prx(${_mod}
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.prx"
        SIGN)

    # --- the loader ---------------------------------------------------------
    set(_ldr vsh-plugin-loader${_sfx})
    add_executable(${_ldr} loader/main.c)
    target_compile_features(${_ldr} PRIVATE c_std_11)
    target_include_directories(${_ldr} PRIVATE include)
    target_compile_options(${_ldr} PRIVATE -Wall -Wextra)
    target_compile_definitions(${_ldr} PRIVATE MODULE_STEM="${_stem}")
    if(_abi STREQUAL "lp64")
        target_compile_options(${_ldr} PRIVATE -mlp64)
        target_link_options(${_ldr} PRIVATE -mlp64)
    endif()
    target_link_libraries(${_ldr} PRIVATE rt lv2)
    ps3_add_self(${_ldr} OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

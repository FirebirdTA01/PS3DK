# Links the ps3dk-run protocol-2 run hook into an unmodified sample.
#
# Configure a sample with
#   -DCMAKE_PROJECT_INCLUDE=<this file> -DPS3DK_RUN_HOOK_DIR=<dir>
# where <dir> is a checkout of spike/ps3dk-run (the directory holding hook/
# and common/).  At the end of the sample's CMakeLists.txt, every executable
# target defined there gets:
#   - its own sources compiled with main renamed to ps3dk_sample_main;
#   - hook_main.c (or .cpp for a C++ sample) as the new main, which starts
#     the hook and then calls the sample's main;
#   - the hook sources, libnet and the sysmodule stubs.
# The sample's sources are not changed.  Without the loader's marker in argv
# the hook stays inert, so the same SELF also boots in RPCS3.
if(NOT PS3DK_RUN_HOOK_DIR)
    message(FATAL_ERROR "run-hook-inject.cmake: set PS3DK_RUN_HOOK_DIR")
endif()
if(NOT EXISTS "${PS3DK_RUN_HOOK_DIR}/hook/ps3dk_run_hook.c")
    message(FATAL_ERROR "run-hook-inject.cmake: no hook/ps3dk_run_hook.c in ${PS3DK_RUN_HOOK_DIR}")
endif()

# The hook is C; C++-only samples need the language too.
enable_language(C)

set(_ps3dk_inject_dir "${CMAKE_CURRENT_LIST_DIR}")

function(_ps3dk_inject_run_hook)
    # Compile the hook from a copy in the build tree: objects of sources
    # outside the project get paths long enough to break the Windows limit.
    set(hook "${CMAKE_BINARY_DIR}/run-hook")
    file(COPY "${PS3DK_RUN_HOOK_DIR}/hook" "${PS3DK_RUN_HOOK_DIR}/common"
         DESTINATION "${hook}")
    get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    set(injected 0)
    foreach(t ${targets})
        get_target_property(type ${t} TYPE)
        if(NOT type STREQUAL "EXECUTABLE")
            continue()
        endif()
        get_target_property(sources ${t} SOURCES)
        set(cxx 0)
        foreach(s ${sources})
            if(s MATCHES "\\.(c|cpp|cc|cxx)$")
                set_property(SOURCE "${s}" TARGET_DIRECTORY ${t}
                             APPEND PROPERTY COMPILE_DEFINITIONS "main=ps3dk_sample_main")
                if(s MATCHES "\\.(cpp|cc|cxx)$")
                    set(cxx 1)
                endif()
            endif()
        endforeach()
        if(cxx)
            set(wrapper "${CMAKE_BINARY_DIR}/run-hook/hook_main_${t}.cpp")
        else()
            set(wrapper "${CMAKE_BINARY_DIR}/run-hook/hook_main_${t}.c")
        endif()
        configure_file("${_ps3dk_inject_dir}/hook_main.c" "${wrapper}" COPYONLY)
        target_sources(${t} PRIVATE "${wrapper}"
            "${hook}/hook/ps3dk_run_hook.c"
            "${hook}/common/prn_net.c"
            "${hook}/common/prn_sha256.c"
            "${hook}/common/prn_hmac.c")
        target_include_directories(${t} PRIVATE "${hook}/hook")
        # libnet loads its sysmodule through the sysmodule stubs.
        target_link_libraries(${t} PRIVATE net sysmodule_stub)
        math(EXPR injected "${injected} + 1")
    endforeach()
    if(injected EQUAL 0)
        message(FATAL_ERROR "run-hook-inject.cmake: no executable target in ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    message(STATUS "run-hook-inject: hook linked into ${injected} target(s)")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _ps3dk_inject_run_hook)

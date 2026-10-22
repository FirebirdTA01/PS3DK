cmake_minimum_required(VERSION 3.20)
project(eh_fde_registry_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "eh-fde-registry: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

foreach(abi ilp32 lp64)
    add_executable(eh-fde-registry-${abi} main.cpp)
    target_compile_features(eh-fde-registry-${abi} PRIVATE cxx_std_17)
    target_compile_options(eh-fde-registry-${abi} PRIVATE -O2 -Wall -Wextra)
    # malloc is wrapped to count calls made before the runtime reports the heap ready; the pthread mutex calls are
    # wrapped for the lazy-sort witness (only calls made inside its marked _Unwind_Find_FDE probes are observed)
    target_link_options(eh-fde-registry-${abi} PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=pthread_mutex_lock"
        "-Wl,--wrap=pthread_mutex_unlock")
    if(abi STREQUAL "lp64")
        target_compile_options(eh-fde-registry-${abi} PRIVATE -mlp64)
        target_link_options(eh-fde-registry-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(eh-fde-registry-${abi})
endforeach()

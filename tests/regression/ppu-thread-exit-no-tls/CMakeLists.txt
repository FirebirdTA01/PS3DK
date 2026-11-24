cmake_minimum_required(VERSION 3.20)
project(ppu_thread_exit_no_tls_regression CXX)

# sys_ppu_thread_exit on a thread with no TLS area (card t_cfb77564): a
# thread the kernel creates without one, the way Cobra runs a VSH plugin's
# module_start, must end through the bare exit system call and hand its
# exit code to the joiner.  Threads with TLS keep the exit cleanup.  See
# main.cpp for the cases.

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "ppu-thread-exit-no-tls: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(STATUS "ppu-thread-exit-no-tls: skipped on GCC ${CMAKE_CXX_COMPILER_VERSION} (no libstdc++ thread layer)")
    return()
endif()

include(ps3-self)

foreach(abi ilp32 lp64)
    add_executable(ppu-thread-exit-no-tls-${abi} main.cpp)
    target_compile_features(ppu-thread-exit-no-tls-${abi} PRIVATE cxx_std_17)
    target_compile_options(ppu-thread-exit-no-tls-${abi} PRIVATE -Wall -Wextra)
    if(abi STREQUAL "lp64")
        target_compile_options(ppu-thread-exit-no-tls-${abi} PRIVATE -mlp64)
        target_link_options(ppu-thread-exit-no-tls-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(ppu-thread-exit-no-tls-${abi})
endforeach()

cmake_minimum_required(VERSION 3.20)
project(pthread_raw_threads_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "pthread-raw-threads: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()
# The C++ thread library needs libstdc++'s posix thread layer, which only the
# GCC 16 toolchain has; 12.4 stays single-threaded until the switchover.
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(STATUS "pthread-raw-threads: skipped on GCC ${CMAKE_CXX_COMPILER_VERSION} (no libstdc++ thread layer)")
    return()
endif()

include(ps3-self)

foreach(abi ilp32 lp64)
    add_executable(pthread-raw-threads-${abi} main.cpp)
    target_compile_features(pthread-raw-threads-${abi} PRIVATE cxx_std_17)
    target_compile_options(pthread-raw-threads-${abi} PRIVATE -Wall -Wextra)
    if(abi STREQUAL "lp64")
        target_compile_options(pthread-raw-threads-${abi} PRIVATE -mlp64)
        target_link_options(pthread-raw-threads-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(pthread-raw-threads-${abi})
endforeach()

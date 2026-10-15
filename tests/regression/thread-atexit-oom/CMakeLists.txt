cmake_minimum_required(VERSION 3.20)
project(thread_atexit_oom_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Host cannot validate Lv-2 pthreads" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()

include(ps3-self)

# The code under test is librt's pthread.c itself, built into the row with its
# allocation hook (hook.h): the row's definitions take the place of
# librt's pthread.o in the link.
set(_pthread_c "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime/lv2/librt/pthread.c")
foreach(abi ilp32 lp64)
    set(_t thread-atexit-oom-${abi})
    add_executable(${_t} main.c "${_pthread_c}")
    target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime/lv2/librt")
    target_compile_features(${_t} PRIVATE c_std_11)
    target_compile_options(${_t} PRIVATE -Werror=implicit-function-declaration -include "${CMAKE_CURRENT_SOURCE_DIR}/hook.h")
    if(abi STREQUAL "lp64")
        target_compile_options(${_t} PRIVATE -mlp64)
        target_link_options(${_t} PRIVATE -mlp64)
    endif()
    ps3_add_self(${_t})
endforeach()

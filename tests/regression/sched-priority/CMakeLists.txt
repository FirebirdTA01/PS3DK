cmake_minimum_required(VERSION 3.20)
project(sched_priority_regression C CXX)

option(PS3TC_REGRESSION_HOST_STUB "Host cannot validate Lv-2 scheduling" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).  Both a C TU and a C++ TU: cxx_linkage.cpp is the
# C++/C-linkage check that the two sched prototypes are visible with
# extern "C" semantics regardless of whether <pthread.h> precedes
# <sched.h>.
foreach(abi ilp32 lp64)
    add_executable(sched-priority-${abi} main.c cxx_linkage.cpp)
    target_compile_features(sched-priority-${abi} PRIVATE c_std_11 cxx_std_17)
    target_compile_options(sched-priority-${abi} PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
    if(abi STREQUAL "lp64")
        target_compile_options(sched-priority-${abi} PRIVATE -mlp64)
        target_link_options(sched-priority-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(sched-priority-${abi})
endforeach()

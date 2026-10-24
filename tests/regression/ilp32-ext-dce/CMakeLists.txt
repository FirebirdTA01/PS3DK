cmake_minimum_required(VERSION 3.20)
project(ilp32_ext_dce_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Host cannot validate Lv-2 pthreads" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
# walk.c at -O2 whatever the build type: the miscompile needs the loop optimisers.
set_source_files_properties(walk.c derived-pointer-rtl.c PROPERTIES COMPILE_OPTIONS "-O2")

foreach(abi ilp32 lp64)
    add_executable(ilp32-ext-dce-${abi} main.c walk.c derived-pointer-rtl.c)
    target_compile_features(ilp32-ext-dce-${abi} PRIVATE c_std_11)
    target_compile_options(ilp32-ext-dce-${abi} PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
    if(abi STREQUAL "lp64")
        target_compile_options(ilp32-ext-dce-${abi} PRIVATE -mlp64)
        target_link_options(ilp32-ext-dce-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(ilp32-ext-dce-${abi})
endforeach()

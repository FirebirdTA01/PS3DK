cmake_minimum_required(VERSION 3.20)

project(tls_ie_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect is PPU initial-exec TLS codegen; a host build tests nothing.
    message(STATUS "tls-ie: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# ie.c and ie-o0.c read def.c's thread-locals initial-exec (GCC patch
# 0057).  --no-tls-optimize keeps the GOT TP-offset load instead of relaxing
# it to local-exec, so the row exercises the slot load itself.
add_executable(tls-ie main.c def.c ie.c ie-o0.c)
target_compile_options(tls-ie PRIVATE -O2 -Wall)
set_source_files_properties(ie-o0.c PROPERTIES COMPILE_OPTIONS -O0)
target_link_options(tls-ie PRIVATE -Wl,--no-tls-optimize)
ps3_add_self(tls-ie)

cmake_minimum_required(VERSION 3.20)

project(librt_posix_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)
add_executable(librt-posix main.c)
target_compile_features(librt-posix PRIVATE c_std_11)

if(PS3TC_REGRESSION_HOST_STUB)
    target_compile_definitions(librt-posix PRIVATE PS3TC_REGRESSION_HOST_STUB=1)
else()
    include(ps3-self)
    target_compile_definitions(librt-posix PRIVATE PS3DK_SUPPRESS_PSL1GHT_COMPAT_WARN=1)
    target_link_libraries(librt-posix PRIVATE net sysmodule)
    ps3_add_self(librt-posix)
endif()

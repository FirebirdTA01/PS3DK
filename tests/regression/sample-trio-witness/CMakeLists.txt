cmake_minimum_required(VERSION 3.20)
project(sample_trio_witness C)
option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    # The witnesses run the samples' own code on lv2.
    message(STATUS "sample-trio-witness: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()
include(ps3-self)

# Fault witnesses for three samples: each case builds the sample's main.c
# with fakes for the calls it makes (see the .c files for the cases).
set(_samples "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples")
set(_cases
    memcontainer:0:control memcontainer:1:ok-null
    input:0:absent input:1:idle input:2:unplug-race input:3:kb-init-fail
    input:4:dispatch-fail input:5:read-fail input:6:end-fail
    cellfs:0:control cellfs:1:scratch-exists cellfs:2:stat-eio
    cellfs:3:req0-timeout cellfs:4:cancel-stuck cellfs:5:close-fail
    cellfs:6:finish-fail)

foreach(abi ilp32 lp64)
    foreach(entry IN LISTS _cases)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 sample)
        list(GET parts 1 num)
        list(GET parts 2 name)
        set(target sample-trio-witness-${sample}-${name}-${abi})
        add_executable(${target} ${sample}.c)
        target_compile_features(${target} PRIVATE c_std_11)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        target_include_directories(${target} PRIVATE "${_samples}")
        target_compile_definitions(${target} PRIVATE
            WITNESS_CASE=${num} WITNESS_NAME="${sample}-${name}"
            MAX_FRAME=20 AIO_TIMEOUT_MS=500)
        if(abi STREQUAL "lp64")
            target_compile_options(${target} PRIVATE -mlp64)
            target_link_options(${target} PRIVATE -mlp64)
        endif()
        target_link_libraries(${target} PRIVATE rt lv2)
        ps3_add_self(${target})
    endforeach()
endforeach()

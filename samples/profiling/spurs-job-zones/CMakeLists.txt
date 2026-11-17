cmake_minimum_required(VERSION 3.20)
project(spurs_job_zones C CXX ASM)
include(ps3-self)
include("${CMAKE_CURRENT_LIST_DIR}/../prof-ext-abi.cmake")

option(SJ_PROFILING "Explicit SPU zones (OFF is a checksum/clock control)" ON)
set(SJ_ACTIVE_SPUS "1" CACHE STRING "Enabled queue SPUs: 1 or 2; actual overlap is not asserted")
set(SJ_BUILD_ID "" CACHE STRING "Pinned 64-character lowercase hexadecimal build identity")
set(SJ_EXT_ARCHIVE "" CACHE FILEPATH "Pinned uninstalled PPU extension archive matching the selected ABI")
get_filename_component(SJ_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SJ_PROF_SRC "${SJ_REPO}/sdk/libps3dkprof/src" CACHE PATH "Reviewed profiler source tree")
set(SJ_JOB_LDSCRIPT "${SJ_REPO}/sdk/libspurs_job/scripts/spurs_job.ld" CACHE FILEPATH "Reviewed independent job linker script")
string(LENGTH "${SJ_BUILD_ID}" _id_len)
if(NOT SJ_BUILD_ID MATCHES "^[0-9a-f]+$" OR NOT _id_len EQUAL 64)
    message(FATAL_ERROR "SJ_BUILD_ID must be exactly 64 lowercase hexadecimal characters")
endif()
if(NOT SJ_ACTIVE_SPUS MATCHES "^[12]$")
    message(FATAL_ERROR "SJ_ACTIVE_SPUS must be 1 or 2")
endif()
prof_ext_compiled_abi(SJ_ABI)
prof_ext_require_abi(${SJ_ABI} "SJ_EXT_ARCHIVE" "${SJ_EXT_ARCHIVE}")
if(SJ_PROFILING)
    set(_sj_on 1)
else()
    set(_sj_on 0)
endif()
set(_path_maps "-ffile-prefix-map=${SJ_REPO}=." "-fmacro-prefix-map=${SJ_REPO}=.")
add_executable(spurs-job-zones source/main.cpp)
target_compile_features(spurs-job-zones PRIVATE cxx_std_17)
target_compile_options(spurs-job-zones PRIVATE -O2 -g -Wall -Wextra -Werror ${_path_maps})
target_include_directories(spurs-job-zones PRIVATE common "${SJ_PROF_SRC}/../../include" "${SJ_PROF_SRC}/spu")
target_compile_definitions(spurs-job-zones PRIVATE PS3DK_PROF=1 SJ_PROFILING=${_sj_on}
    SJ_ACTIVE_SPUS=${SJ_ACTIVE_SPUS} "SJ_BUILD_ID=\"${SJ_BUILD_ID}\"")
target_link_options(spurs-job-zones PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/spurs-job-zones.map")
# The extension archive also carries its GCM port, even with GPU domains off.
# Resolve its RSX/GCM references with the same providers as ext-probe.
target_link_libraries(spurs-job-zones PRIVATE "${SJ_EXT_ARCHIVE}" spurs_jq_stub spurs_stub c_stub gcm_cmd gcm_sys sysmodule_stub sysutil rt lv2 m)

# Sample-local linker augmentation. No SDK file is edited or installed.
# The driver supplies -dT for its default script; our explicit -T replaces it.
# Refuse an unfamiliar source shape instead of silently retaining an orphan.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SJ_JOB_LDSCRIPT}")
file(READ "${SJ_JOB_LDSCRIPT}" _job_script)
string(REGEX MATCHALL "[.]fixup : ALIGN[(]16[)]" _anchors "${_job_script}")
list(LENGTH _anchors _anchor_count)
if(NOT _anchor_count EQUAL 1)
    message(FATAL_ERROR "SJ_JOB_LDSCRIPT must have exactly one .fixup : ALIGN(16) anchor")
endif()
if(_job_script MATCHES "ps3dkprof_spudesc")
    message(FATAL_ERROR "Review the sample linker augmentation: input already has profiler descriptors")
endif()
set(_desc_section "ps3dkprof_spudesc : ALIGN(16) {\n        __start_ps3dkprof_spudesc = .;\n        KEEP(*(ps3dkprof_spudesc))\n        __stop_ps3dkprof_spudesc = .;\n    } :text\n\n    .fixup : ALIGN(16)")
string(REPLACE ".fixup : ALIGN(16)" "${_desc_section}" _job_script "${_job_script}")
set(_sample_script "${CMAKE_CURRENT_BINARY_DIR}/spurs-prof-job.ld")
file(WRITE "${_sample_script}" "${_job_script}")

set(_producer_sources)
if(SJ_PROFILING)
    list(APPEND _producer_sources "${SJ_PROF_SRC}/spu/psp_spu.c" "${SJ_PROF_SRC}/spu/prof_spu_writer.c")
endif()
ps3_add_spu_image(spurs-job-zones NAME spurs_prof_job JOBBIN_WRAP
    SOURCES spu/job.c spu/job_mem.c ${_producer_sources}
    LDSCRIPT "${_sample_script}"
    CFLAGS -mspurs-job-initialize -mno-error-reloc -Wextra -Werror -Wno-cpp -std=gnu11
           "-DSJ_PROFILING=${_sj_on}" "-DPS3DK_PROF=${_sj_on}" -Dmemset=sj_memset
           "-I${CMAKE_CURRENT_LIST_DIR}/common" "-I${SJ_PROF_SRC}/spu" ${_path_maps}
    LDFLAGS -mspurs-job-initialize -Wl,-q "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/spurs-prof-job.map")

# The existing helper's custom compile rules depend on translation units only.
# Carry this sample's protocol/producer header edits into incremental builds.
file(GLOB _producer_headers "${SJ_PROF_SRC}/spu/*.h")
set(_spu_objects job.c job_mem.c)
if(SJ_PROFILING)
    list(APPEND _spu_objects psp_spu.c prof_spu_writer.c)
endif()
foreach(_unit IN LISTS _spu_objects)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/spu/spurs_prof_job/${_unit}.o" APPEND
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/common/protocol.h" ${_producer_headers})
endforeach()

if(SJ_PROFILING)
    # Keep the execution raw ELF/JOBBIN_WRAP/.fixup unchanged. The strict
    # registration parser rejects -q's retained descriptor RELA metadata.
    # Validate all already-applied sites before nulling only that non-loadable
    # section header in a separate, independently identified registration copy.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_raw "${CMAKE_CURRENT_BINARY_DIR}/spu/spurs_prof_job/spurs_prof_job.bin")
    set(_registration "${CMAKE_CURRENT_BINARY_DIR}/spurs_prof_job_registration.bin")
    set(_registration_receipt "${_registration}.json")
    set(_registration_tool "${CMAKE_CURRENT_LIST_DIR}/tools/registration_elf.py")
    add_custom_command(OUTPUT "${_registration}" "${_registration_receipt}"
        COMMAND "${Python3_EXECUTABLE}" -B "${_registration_tool}" "${_raw}" "${_registration}"
                --receipt "${_registration_receipt}" --replace
        DEPENDS "${_raw}" "${_registration_tool}" VERBATIM)
    ps3_bin2s(spurs-job-zones "${_registration}")
endif()
ps3_add_self(spurs-job-zones)

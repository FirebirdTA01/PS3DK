# spurs-suite row helpers, shared by the regression suite (one self per
# row) and the on-screen status sample (SUITE_EMBEDDED: every row linked
# into one program as an entry function).  The includer sets SUITE_DIR to
# this directory.
#
# A row's CMakeLists calls suite_ppu() first; it sets SUITE_SPU, the row's
# SPU image name (unique per row, so rows can share one program), and the
# PPU source reaches the image through SUITE_SPU_HEADER / SUITE_SPU_BIN /
# SUITE_SPU_BIN_SIZE / SUITE_SPU_JOBHEADER(_HEADER).  suite_self() ends the
# row.
set(SUITE_PPU_LIBS spurs_stub c_stub sysmodule_stub sysutil rt lv2 m)

function(suite_ppu target source)
    string(MAKE_C_IDENTIFIER "${target}" _id)
    set(_spu "${_id}_spu")
    if(SUITE_EMBEDDED)
        add_library(${target} OBJECT ${SUITE_DIR}/ppu/${source})
        target_compile_definitions(${target} PRIVATE SUITE_EMBEDDED "SUITE_ENTRY=suite_entry_${_id}")
        set_property(GLOBAL APPEND PROPERTY SUITE_ROWS ${target})
        set_property(GLOBAL APPEND PROPERTY SUITE_ROW_LIBS ${ARGN})
    else()
        add_executable(${target} ${SUITE_DIR}/ppu/${source})
        target_link_libraries(${target} PRIVATE ${ARGN} ${SUITE_PPU_LIBS})
    endif()
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_compile_definitions(${target} PRIVATE
        "SUITE_ROW=\"${target}\""
        "SUITE_SPU_HEADER=\"${_spu}_bin.h\""
        "SUITE_SPU_BIN=${_spu}_bin"
        "SUITE_SPU_BIN_SIZE=${_spu}_bin_size"
        "SUITE_SPU_JOBHEADER=${_spu}_jobheader_bin"
        "SUITE_SPU_JOBHEADER_HEADER=\"${_spu}_jobheader_bin.h\"")
    set(SUITE_SPU ${_spu} PARENT_SCOPE)
endfunction()

# SPURS tasks linked by hand: task startup, runtime and layout named
# explicitly; extra SPU libraries follow the source
function(suite_task_manual target spu_source)
    ps3_add_spu_image(${target}
        NAME ${SUITE_SPU}
        SOURCES ${SUITE_DIR}/spu/${spu_source}
        CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp
        NOSTARTFILES
        FREESTANDING
        LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
        LIBS spurs_task ${ARGN})
endfunction()

function(suite_self target)
    if(NOT SUITE_EMBEDDED)
        ps3_add_self(${target})
    endif()
endfunction()

# the rows, in run order
set(SUITE_ROW_DIRS
    spurs-task-manual spurs-task-driver
    spurs-event-flag spurs-event-flag-driver
    spurs-queue spurs-lfqueue
    spurs-control spurs-barrier
    spurs-job-chain-manual spurs-job-chain-driver
    spurs-job-queue-manual spurs-job-queue-driver)

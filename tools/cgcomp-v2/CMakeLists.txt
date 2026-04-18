# cgcomp-v2 — Phase 8 Cg → NV40 compiler (build skeleton)
#
# Stage 0: declares the project shape; does not yet build a binary.
# Stage 1+ progressively populates ${SRC_NV40} with the new back-end and
# wires in the donor front-end from $VITA_CG_ROOT.

cmake_minimum_required(VERSION 3.16)
project(cgcomp-v2
    LANGUAGES CXX
    DESCRIPTION "PS3 Cg-to-NV40 shader compiler (Sony binary-compat)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Donor front-end source root.  Cloned by scripts/bootstrap.sh into
# $PS3_TOOLCHAIN_ROOT/src/vita-cg-compiler/.  Override with -DVITA_CG_ROOT=...
# if you have it elsewhere.
set(VITA_CG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita-cg-compiler"
    CACHE PATH "Path to vita-cg-compiler source tree (front-end donor)")

set(VITA_CG_SRC "${VITA_CG_ROOT}/src")

if(NOT EXISTS "${VITA_CG_SRC}/frontend/parser.h")
    message(WARNING
        "vita-cg-compiler not found at ${VITA_CG_ROOT}.\n"
        "Run scripts/bootstrap.sh to clone it, or pass -DVITA_CG_ROOT=...")
endif()

# ----------------------------------------------------------------------------
# Source lists.  Empty placeholders for Stage 0; filled in as later stages land.
# ----------------------------------------------------------------------------

# Donor front-end (vita-cg-compiler/src/{frontend,ir,common,builtin_*}).
# Listed but commented out until Stage 1 wires them in.  This makes the
# expected boundaries explicit.
set(SRC_FRONTEND_DONOR
    # ${VITA_CG_SRC}/common/errorHandler.cpp
    # ${VITA_CG_SRC}/common/memoryPool.cpp
    # ${VITA_CG_SRC}/frontend/preprocessor.cpp
    # ${VITA_CG_SRC}/frontend/lexer.cpp
    # ${VITA_CG_SRC}/frontend/parser.cpp
    # ${VITA_CG_SRC}/frontend/semantic.cpp
    # ${VITA_CG_SRC}/frontend/symbol_table.cpp
    # ${VITA_CG_SRC}/frontend/types.cpp
    # ${VITA_CG_SRC}/ir/ir_builder.cpp
    # ${VITA_CG_SRC}/ir/ir_passes.cpp
    # ${VITA_CG_SRC}/builtin_shader_header.cpp
)

# NEW: NV40 back-end (this tree).  Populated in Stage 2.
set(SRC_NV40
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/nv40_lowering.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/nv40_regalloc.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/nv40_emit.cpp
)

# NEW: Sony container emitter (this tree).  Populated in Stage 4.
set(SRC_CONTAINER
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/sony_container_fp.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/sony_container_vp.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/psl1ght_container_fp.cpp  # optional compat
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/psl1ght_container_vp.cpp  # optional compat
)

# Driver / entry point.  Populated in Stage 1.
set(SRC_MAIN
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
)

set(CGCOMP_V2_SOURCES
    ${SRC_FRONTEND_DONOR}
    ${SRC_NV40}
    ${SRC_CONTAINER}
    ${SRC_MAIN}
)

if(CGCOMP_V2_SOURCES)
    add_executable(cgcomp-v2 ${CGCOMP_V2_SOURCES})
    target_include_directories(cgcomp-v2 PRIVATE
        ${VITA_CG_SRC}
        ${VITA_CG_SRC}/frontend
        ${VITA_CG_SRC}/ir
        ${VITA_CG_SRC}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
else()
    # Stage 0: nothing to build yet.  Surface the shape via a custom target
    # so `cmake .. && make` succeeds with a clear message.
    add_custom_target(cgcomp-v2-stage0-placeholder
        COMMAND ${CMAKE_COMMAND} -E echo
            "cgcomp-v2: Stage 0 skeleton.  Source lists empty until Stage 1+.")
endif()

cmake_minimum_required(VERSION 3.20)
project(sample-sprx-module-export C ASM)

include(ps3-self)

# If building in-tree, include repository SDK headers so newly added headers
# (such as <sys/moduleexport.h>) are available even when building against
# a pre-installed SDK baseline.
get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(IS_DIRECTORY "${_repo_root}/sdk/include")
    include_directories(SYSTEM "${_repo_root}/sdk/include")
endif()

# -----------------------------------------------------------------------------
# Module Target
# Declares module identity and entry points in source code via SYS_MODULE_INFO,
# SYS_MODULE_START, and SYS_MODULE_STOP from <sys/moduleexport.h>.
# Output is placed under CMAKE_CURRENT_BINARY_DIR so the source tree remains clean.
# -----------------------------------------------------------------------------
add_executable(sample-module-export
    module/export.c
    module/exports.S)
target_compile_features(sample-module-export PRIVATE c_std_11)
target_include_directories(sample-module-export PRIVATE include)

ps3_add_prx(sample-module-export
    EXPORTS exports.yml
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sample_module_export.prx"
    SIGN)

ps3_prx_export_roundtrip(sample-module-export)

# -----------------------------------------------------------------------------
# Stub Archive Target
# Builds client import stub archive from exports.yml.
# -----------------------------------------------------------------------------
ps3_prx_stub_library(sample_module_export_stub
    EXPORTS exports.yml
    ARCHIVE_NAME sample_module_export)

# -----------------------------------------------------------------------------
# Host Loader Target
# Loads the signed SPRX module, verifies start handler execution and handoff,
# invokes exported functions, verifies stop handler execution, and unloads.
# -----------------------------------------------------------------------------
add_executable(sample-module-loader
    loader/main.c)
target_compile_features(sample-module-loader PRIVATE c_std_11)
target_include_directories(sample-module-loader PRIVATE include)
target_link_libraries(sample-module-loader PRIVATE sample_module_export_stub rt lv2)

target_compile_definitions(sample-module-loader PRIVATE
    MODULE_PRX_PATH="/app_home/sample_module_export.fake.sprx")

ps3_add_self(sample-module-loader
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

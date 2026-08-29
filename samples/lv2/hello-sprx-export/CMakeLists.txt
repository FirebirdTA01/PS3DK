cmake_minimum_required(VERSION 3.20)
project(hello-sprx-export C ASM)

include(ps3-self)

add_executable(hello-sprx-export
    source/export.c
    source/exports.S)
target_compile_features(hello-sprx-export PRIVATE c_std_11)
target_include_directories(hello-sprx-export PRIVATE include)

ps3_add_prx(hello-sprx-export
    NAME hello_prx
    VERSION 1.0
    EXPORTS exports.yml
    OUTPUT_NAME hello-sprx-export.prx)

# Prove the export round trip: read this module's own export table back out
# and check it still describes exports.yml.
ps3_prx_export_roundtrip(hello-sprx-export)

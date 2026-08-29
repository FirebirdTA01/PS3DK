cmake_minimum_required(VERSION 3.20)
project(hello-sprx-import C)

include(ps3-self)

ps3_prx_stub_library(hello_prx_stub
    EXPORTS ../hello-sprx-export/exports.yml
    ARCHIVE_NAME hello_prx)

add_executable(hello-sprx-import source/main.c)
target_compile_features(hello-sprx-import PRIVATE c_std_11)
target_include_directories(hello-sprx-import PRIVATE ../hello-sprx-export/include)
target_link_libraries(hello-sprx-import PRIVATE hello_prx_stub rt lv2)

ps3_add_self(hello-sprx-import)

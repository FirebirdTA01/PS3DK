cmake_minimum_required(VERSION 3.20)
project(hello-cellcgb-parse C)

include(ps3-self)

add_executable(hello-cellcgb-parse source/main.c)
target_link_libraries(hello-cellcgb-parse PRIVATE cgb sysutil_stub sysutil m)

ps3_add_self(hello-cellcgb-parse)

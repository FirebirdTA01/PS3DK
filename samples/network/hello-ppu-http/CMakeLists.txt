cmake_minimum_required(VERSION 3.20)
project(hello-ppu-http CXX)

include(ps3-self)

add_executable(hello-ppu-http source/main.cpp)
target_compile_features(hello-ppu-http PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-http PRIVATE
    net_stub http_stub http_util_stub ssl_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-http)

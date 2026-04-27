# hello-ppu-savedata-link — link-only test for the cellSaveData
# Sony-name surface.  Confirms all 39 entry points resolve at link
# time against libsysutil + libsysutil_savedata_stub +
# libsysutil_savedata_extra_stub.  Real save/load callback flows
# would need user content + RPCS3 setup, which is outside this
# header-vs-archive coverage check.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-savedata-link C)

include(ps3-self)

add_executable(hello-ppu-savedata-link source/main.c)
target_compile_features(hello-ppu-savedata-link PRIVATE c_std_11)

target_link_libraries(hello-ppu-savedata-link PRIVATE
    sysutil_savedata_stub sysutil_savedata_extra_stub
    sysutil sysmodule_stub io rt lv2
)

ps3_add_self(hello-ppu-savedata-link)

# hello-sdata - developer SDATA files read through cellFsSdataOpen.
#
# The build wraps one payload into SDATA files of each layout make_sdata
# writes (current format, both old formats, 1 KB blocks, the compressed
# layout) and embeds them with the plain payload.  The program writes each
# file to /dev_hdd0/tmp, opens it with cellFsSdataOpen and checks it reads
# back as the payload.
#
# Build:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(hello-sdata C)

include(ps3-self)

# 40000 bytes of numbered 50-byte lines: three 16 KB blocks, the last one
# short, and no two blocks alike
string(ASCII 10 _nl)
set(_payload "")
foreach(_i RANGE 0 799)
    string(LENGTH "${_i}" _digits)
    math(EXPR _pad "4 - ${_digits}")
    string(REPEAT "0" ${_pad} _zeros)
    string(APPEND _payload "line ${_zeros}${_i} of the hello-sdata payload ..........${_nl}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/payload.bin.new" "${_payload}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/payload.bin.new" "${CMAKE_CURRENT_BINARY_DIR}/payload.bin" COPYONLY)
set(_in "${CMAKE_CURRENT_BINARY_DIR}/payload.bin")

add_executable(hello-sdata source/main.c)
target_compile_features(hello-sdata PRIVATE c_std_11)
target_link_libraries(hello-sdata PRIVATE fs_stub sysutil rt lv2)

ps3_make_sdata(data_v4.sdat "${_in}")
ps3_make_sdata(data_v3.sdat "${_in}" FORMAT 3)
ps3_make_sdata(data_v2.sdat "${_in}" FORMAT 2)
ps3_make_sdata(data_b1.sdat "${_in}" BLOCK_KB 1)
ps3_make_sdata(data_z.sdat "${_in}" COMPRESS)
ps3_bin2s(hello-sdata "${_in}")
foreach(_v v4 v3 v2 b1 z)
    ps3_bin2s(hello-sdata "${CMAKE_CURRENT_BINARY_DIR}/data_${_v}.sdat")
endforeach()

ps3_add_self(hello-sdata)

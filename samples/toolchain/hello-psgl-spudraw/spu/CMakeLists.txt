cmake_minimum_required(VERSION 3.20)
project(hello-psgl-spudraw-spu C)

set(CMAKE_SYSTEM_PROCESSOR spu)
set(CMAKE_C_COMPILER spu-elf-gcc)

# Inline build for SPU probe task. Not used directly;
# ps3_add_spu_image in the parent CMakeLists handles the build.
add_executable(spu_hole_fill source/spu_hole_fill.c)
target_compile_features(spu_hole_fill PRIVATE c_std_11)
target_compile_options(spu_hole_fill PRIVATE -ffreestanding -fno-exceptions -O2)
target_include_directories(spu_hole_fill PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../sdk/include-spu
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../stage/ps3dev/ps3dk/spu/include)
target_link_libraries(spu_hole_fill PRIVATE
    -L${CMAKE_CURRENT_SOURCE_DIR}/../../../../stage/ps3dev/ps3dk/spu/lib
    -lspuPSGL -lsputhread -lgcc)

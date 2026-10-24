cmake_minimum_required(VERSION 3.20)
project(spu_lowpart_spill_regression C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)
# Extra SPU flags for A/B runs (for example -flate-combine-instructions or
# -fno-late-combine-instructions); empty in the normal row.
set(SPU_LOWPART_SPILL_EXTRA_CFLAGS "" CACHE STRING "Extra SPU CFLAGS for A/B runs")
separate_arguments(spu_lowpart_spill_extra NATIVE_COMMAND "${SPU_LOWPART_SPILL_EXTRA_CFLAGS}")
add_executable(spu-lowpart-spill main.c)
target_compile_features(spu-lowpart-spill PRIVATE c_std_11)
target_compile_options(spu-lowpart-spill PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(spu-lowpart-spill PRIVATE lv2)
# -ffixed-126: the PIC register, which the cases' asm may not clobber, so
# no value can stay in it across the asm (spu/main.c).
ps3_add_spu_image(spu-lowpart-spill
    NAME spu
    SOURCES spu/main.c
    CFLAGS -O2 -ffixed-126 -Wall -Wextra -Werror ${spu_lowpart_spill_extra})
ps3_add_self(spu-lowpart-spill)

# a custom policy module: the PPU adds it as a workload, the kernel runs it;
# it loads and runs a relocatable -mcustom-module work unit
suite_ppu(spurs-module module.cpp)
ps3_add_spu_image(spurs-module
    NAME ${SUITE_SPU}_pm
    SOURCES ${SUITE_DIR}/spu/module_pm.c
    NOSTARTFILES
    JOBBIN
    LDFLAGS -Wl,-Ttext=0xa00 -Wl,-e,cellSpursModuleEntry -Wl,--defsym=_start=cellSpursModuleEntry
    LIBS spurs_task)
ps3_add_spu_image(spurs-module
    NAME ${SUITE_SPU}_unit
    SOURCES ${SUITE_DIR}/spu/module_unit.cpp
    JOBBIN
    CFLAGS -mcustom-module
    LDFLAGS -mcustom-module -Wl,-e,unit_entry -Wl,--defsym=_start=unit_entry)
target_compile_definitions(spurs-module PRIVATE
    "SUITE_UNIT_HEADER=\"${SUITE_SPU}_unit_bin.h\""
    "SUITE_UNIT_BIN=${SUITE_SPU}_unit_bin"
    "SUITE_UNIT_BIN_SIZE=${SUITE_SPU}_unit_bin_size")
target_compile_definitions(spurs-module PRIVATE
    "SUITE_PM_HEADER=\"${SUITE_SPU}_pm_bin.h\""
    "SUITE_PM_BIN=${SUITE_SPU}_pm_bin"
    "SUITE_PM_BIN_SIZE=${SUITE_SPU}_pm_bin_size")
suite_self(spurs-module)

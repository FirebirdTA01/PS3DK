# a custom policy module: the PPU adds it as a workload, the kernel runs it
suite_ppu(spurs-module module.cpp)
ps3_add_spu_image(spurs-module
    NAME ${SUITE_SPU}_pm
    SOURCES ${SUITE_DIR}/spu/module_pm.c
    NOSTARTFILES
    JOBBIN
    LDFLAGS -Wl,-Ttext=0xa00 -Wl,-e,cellSpursModuleEntry -Wl,--defsym=_start=cellSpursModuleEntry
    LIBS spurs_task)
target_compile_definitions(spurs-module PRIVATE
    "SUITE_PM_HEADER=\"${SUITE_SPU}_pm_bin.h\""
    "SUITE_PM_BIN=${SUITE_SPU}_pm_bin"
    "SUITE_PM_BIN_SIZE=${SUITE_SPU}_pm_bin_size")
suite_self(spurs-module)

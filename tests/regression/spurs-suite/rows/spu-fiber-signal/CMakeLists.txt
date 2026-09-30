# fiber signal: SPURS tasks wake PPU fibers of a fiber utility runtime
suite_ppu(spu-fiber-signal fiber_signal.cpp fiber_stub)
suite_task_manual(spu-fiber-signal fiber_signal.c fiber)
suite_self(spu-fiber-signal)

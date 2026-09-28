# queue: SPU push/pop against the firmware PPU side, all three directions
suite_ppu(spurs-queue queue.cpp)
suite_task_manual(spurs-queue queue.c)
ps3_add_self(spurs-queue)

# lock-free queue: SPU push/pop against the firmware PPU side, all three directions
suite_ppu(spurs-lfqueue lfqueue.cpp sync_stub)
suite_task_manual(spurs-lfqueue lfqueue.c sync)
suite_self(spurs-lfqueue)

# event flag: PPU->SPU, SPU->SPU and SPU->PPU on their blocking paths
suite_ppu(spurs-event-flag event_flag.cpp)
suite_task_manual(spurs-event-flag event_flag.c)
ps3_add_self(spurs-event-flag)

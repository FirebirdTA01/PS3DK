# barrier: tasks notify and wait; the barrier releases them together
suite_ppu(spurs-barrier barrier.cpp)
suite_task_manual(spurs-barrier barrier.cpp)
suite_self(spurs-barrier)

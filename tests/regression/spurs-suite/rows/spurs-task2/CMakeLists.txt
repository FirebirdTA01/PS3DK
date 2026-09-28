# Task2 and exit codes: SPU-side CreateTask2 / WithBinInfo, Join / TryJoin,
# exit-code containers, and the PPU joining the task that did it
suite_ppu(spurs-task2 task2.cpp)
suite_task_manual(spurs-task2 task2.c)
suite_self(spurs-task2)

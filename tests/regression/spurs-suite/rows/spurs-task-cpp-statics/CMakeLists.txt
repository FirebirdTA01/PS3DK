# C++ SPURS task with static objects: the task startup runs their
# constructors (.ctors) and defines __dso_handle for their destructors
suite_ppu(spurs-task-cpp-statics task_statics.cpp)
suite_task_manual(spurs-task-cpp-statics task_statics.cpp)
suite_self(spurs-task-cpp-statics)

# LS patterns: generate, save-area size, the running task's pattern and
# the patterns read from a task ELF
suite_ppu(spurs-ls-pattern ls_pattern.cpp)
suite_task_manual(spurs-ls-pattern ls_pattern.c)
suite_self(spurs-ls-pattern)

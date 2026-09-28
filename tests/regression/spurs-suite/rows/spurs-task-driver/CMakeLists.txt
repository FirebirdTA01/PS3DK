# task services, task built with the -mspurs-task driver mode
suite_ppu(spurs-task-driver task_services.cpp)
ps3_add_spu_image(spurs-task-driver
    NAME spu_task
    SOURCES ${SUITE_DIR}/spu/task_services.c
    CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp -mspurs-task
    LDFLAGS -mspurs-task)
ps3_add_self(spurs-task-driver)

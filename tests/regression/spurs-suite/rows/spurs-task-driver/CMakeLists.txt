# task services, task built with the -mspurs-task driver mode
suite_ppu(spurs-task-driver task_services.cpp)
ps3_add_spu_image(spurs-task-driver
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/task_services.c
    CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp -mspurs-task
    LDFLAGS -mspurs-task)
suite_self(spurs-task-driver)

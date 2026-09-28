# event flag, task built with the -mspurs-task driver mode
suite_ppu(spurs-event-flag-driver event_flag.cpp)
ps3_add_spu_image(spurs-event-flag-driver
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/event_flag.c
    CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp -mspurs-task
    LDFLAGS -mspurs-task)
suite_self(spurs-event-flag-driver)

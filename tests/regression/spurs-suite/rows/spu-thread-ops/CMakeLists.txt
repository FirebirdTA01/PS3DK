# the Lv-2 SPU thread and group calls on a plain SPU thread: LS read and
# write, mailbox, SPU configuration, priority, suspend and resume, user
# events to a PPU event queue
suite_ppu(spu-thread-ops spu_thread_ops.cpp)
ps3_add_spu_image(spu-thread-ops
    NAME ${SUITE_SPU}
    SOURCES ${SUITE_DIR}/spu/spu_thread_ops.c
    CFLAGS -O2 -Wall -Wextra -Werror)
suite_self(spu-thread-ops)

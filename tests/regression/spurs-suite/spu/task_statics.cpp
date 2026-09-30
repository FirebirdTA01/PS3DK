/* spurs-task-cpp-statics row, SPU task: C++ objects with static storage
 * are constructed before cellSpursTaskMain runs (the task startup runs
 * .ctors), in declaration order within the file, and their destructors
 * register against __dso_handle. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/task.h>
#include "../task_statics.h"

namespace {
struct Counter {
    uint32_t value;
    explicit Counter(uint32_t seed) : value(seed * 3) {}
    ~Counter() { value = 0; }            /* registered with __cxa_atexit */
};

uint32_t s_marks, s_order;

struct Mark {
    explicit Mark(uint32_t id) { s_order = (s_order << 8) | id; ++s_marks; }
};

Counter s_counter(TS_SEED);
Mark s_first(1);
Mark s_second(2);
}

static ts_box box;

extern "C" int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t box; uint32_t a2, a3; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;
    box.value = s_counter.value;
    box.marks = s_marks;
    box.order = s_order;
    box.state = 2;
    mfc_put(&box, arg.box, sizeof box, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    return 0;
}

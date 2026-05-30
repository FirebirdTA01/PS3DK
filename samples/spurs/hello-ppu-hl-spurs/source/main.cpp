#include <cstdio>

#include <sys/process.h>
#include <hl/spurs.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    using namespace sce::hl::spurs;

    Spurs *spurs = new Spurs;
    Taskset *taskset = new Taskset;
    if (!spurs || !taskset) {
        return 1;
    }

    int rc = Spurs::initialize(spurs, "HLSpurs", 1, 100, 100);
    std::printf("Spurs::initialize -> 0x%08x\n", rc);
    if (rc == CELL_OK) {
        rc = Taskset::create(taskset, "HLTaskset", spurs, 0, 15, 0x01);
        std::printf("Taskset::create -> 0x%08x\n", rc);
    }
    if (rc == CELL_OK) {
        int shutdown_rc = taskset->shutdown();
        int join_rc = taskset->join();
        std::printf("Taskset shutdown/join -> 0x%08x 0x%08x\n",
                    shutdown_rc, join_rc);
        rc = shutdown_rc ? shutdown_rc : join_rc;
    }
    if (spurs) {
        int finalize_rc = spurs->finalize();
        std::printf("Spurs::finalize -> 0x%08x\n", finalize_rc);
        if (rc == CELL_OK) rc = finalize_rc;
    }

    delete taskset;
    delete spurs;
    return rc == CELL_OK ? 0 : 1;
}

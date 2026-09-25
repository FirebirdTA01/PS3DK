/*
 * spu-header-visibility-test.cpp — SPU header visibility & C++ compatibility
 *
 * Isolated header-first probes:
 * 1. PROBE_JOB_CHAIN: <cell/spurs/job_chain.h> included first without prior
 *    stddef.h or sys/types.h, verifying size_t is declared for operator new.
 * 2. PROBE_SPU_EVENT: <sys/spu_event.h> included first without prior
 *    stddef.h or sys/types.h, verifying size_t is declared for operator new.
 * 3. PROBE_VMX2SPU: <vmx2spu.h> vec_sum2s macro invocation isolated from
 *    size_t probes, verifying spu_and compound literal macro arity.
 */

#if defined(PROBE_JOB_CHAIN)
# include <cell/spurs/job_chain.h>

class SampleJob {
public:
    void *operator new(size_t size, void *p) {
        (void)size;
        return p;
    }
};

#elif defined(PROBE_SPU_EVENT)
# include <sys/spu_event.h>

class SampleEvent {
public:
    void *operator new(size_t size, void *p) {
        (void)size;
        return p;
    }
};

#elif defined(PROBE_VMX2SPU)
# include <spu_intrinsics.h>
# include <vmx2spu.h>

vec_int4 test_vmx2spu_vec_sum2s(vec_int4 a, vec_int4 b)
{
    return vec_sum2s(a, b);
}

#else
# error "Specify -DPROBE_JOB_CHAIN, -DPROBE_SPU_EVENT, or -DPROBE_VMX2SPU"
#endif

int main(void)
{
    return 0;
}

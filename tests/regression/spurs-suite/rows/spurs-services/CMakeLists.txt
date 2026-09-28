# SPU services on the SPURS instance: the "2" task services, workload flag
# receiver, volatile area, SPU GUID, ready counts, workload data,
# priorities / contention, the SPU semaphore initializer, tracing and the
# function forms of the macro-only names
suite_ppu(spurs-services services.cpp)
suite_task_manual(spurs-services services.c)
suite_self(spurs-services)

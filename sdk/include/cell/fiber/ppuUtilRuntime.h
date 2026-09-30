/* cell/fiber/ppuUtilRuntime.h - a ready-made PPU fiber runtime: a fiber
 * utility worker control plus the PPU threads that run its fibers.
 *
 *   Runtime::initialize(rt, prio, n)   load the fiber module (optional),
 *                                      set up the worker control, start n
 *                                      worker threads running its fibers
 *   rt->createFiber(...)               create a fiber on it
 *   rt->wakeup()                       wake a sleeping worker
 *   rt->joinFiber(fiber, &exitCode)    wait for a fiber to finish
 *   rt->shutdown(); rt->finalize();    stop the workers, join them, release
 *
 * SPU code signals the runtime's fibers with the SPU cell/fiber/ppuUtilRuntime.h.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H__
#define __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <sys/ppu_thread.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/fiber/error.h>
#include <cell/fiber/ppu_fiber_types.h>
#include <cell/fiber/ppu_fiber.h>
#include <cell/fiber/ppu_fiber_worker_control.h>
#include <cell/fiber/ppuUtilDefine.h>

#ifdef __cplusplus
__CELL_FIBER_PPU_UTIL_BEGIN

class Runtime : public CellFiberPpuUtilWorkerControl {
public:
	static const uint32_t MAX_WORKER_PPU_THREAD = 16;
	static const uint32_t DEFAULT_NUM_PPU_THREAD = 2;
	static const uint32_t DEFAULT_PPU_THREAD_PRIORITY = 1000;
	/* a worker runs cellFiberPpuUtilWorkerControlRunFibers, which itself
	 * needs about 4 KB of stack: give it headroom */
	static const uint32_t DEFAULT_PPU_THREAD_STACK_SIZE = 16 * 1024;

	Runtime() {}

	static int initialize(Runtime *runtime,
	                      unsigned int priority = DEFAULT_PPU_THREAD_PRIORITY,
	                      unsigned int numWorker = DEFAULT_NUM_PPU_THREAD,
	                      bool autoPrxLoad = true,
	                      bool autoCheckFlags = false,
	                      uint32_t autoCheckFlagsIntervalUsec = 0)
	{
		if (!runtime)
			return CELL_FIBER_ERROR_NULL_POINTER;
		if ((uintptr_t)runtime & 0x7f)
			return CELL_FIBER_ERROR_ALIGN;
		if (numWorker > MAX_WORKER_PPU_THREAD)
			return CELL_FIBER_ERROR_INVAL;
		int rc;
		if (autoPrxLoad && (rc = cellSysmoduleLoadModule(CELL_SYSMODULE_FIBER)) != CELL_OK)
			return rc;
		cellFiberPpuInitialize();

		CellFiberPpuUtilWorkerControlAttribute attr;
		cellFiberPpuUtilWorkerControlAttributeInitialize(&attr);
		attr.scheduler.debuggerSupport = true;
		attr.scheduler.autoCheckFlags = autoCheckFlags;
		attr.scheduler.autoCheckFlagsIntervalUsec = autoCheckFlagsIntervalUsec;
		if ((rc = cellFiberPpuUtilWorkerControlInitializeWithAttribute(runtime, &attr)) != CELL_OK) {
			if (autoPrxLoad)
				cellSysmoduleUnloadModule(CELL_SYSMODULE_FIBER);
			return rc;
		}

		runtime->m_numWorker = numWorker;
		runtime->m_autoLoad = autoPrxLoad;
		runtime->m_lock = 0;
		runtime->m_creating = 0;
		runtime->m_closing = false;
		runtime->m_shutdown = false;
		for (unsigned int i = 0; i < numWorker; i++) {
			char name[28];
			snprintf(name, sizeof name, "_fiberRuntime_%08x_%02u", (unsigned)(uintptr_t)runtime, i);
			rc = sys_ppu_thread_create(&runtime->m_worker[i], worker, (uintptr_t)runtime, priority,
			                           DEFAULT_PPU_THREAD_STACK_SIZE, SYS_PPU_THREAD_CREATE_JOINABLE, name);
			if (rc != CELL_OK) {
				/* undo: stop the workers already started */
				cellFiberPpuUtilWorkerControlWakeup(runtime);
				cellFiberPpuUtilWorkerControlShutdown(runtime);
				for (unsigned int j = 0; j < i; j++) {
					uint64_t exitCode;
					sys_ppu_thread_join(runtime->m_worker[j], &exitCode);
				}
				cellFiberPpuUtilWorkerControlFinalize(runtime);
				if (autoPrxLoad)
					cellSysmoduleUnloadModule(CELL_SYSMODULE_FIBER);
				return rc;
			}
		}
		return CELL_OK;
	}

	int finalize()
	{
		if ((uintptr_t)this & 0x7f)
			return CELL_FIBER_ERROR_ALIGN;
		if (!m_shutdown)
			return CELL_FIBER_ERROR_STAT;
		for (unsigned int i = 0; i < m_numWorker; i++) {
			uint64_t exitCode;
			sys_ppu_thread_join(m_worker[i], &exitCode);
		}
		cellFiberPpuUtilWorkerControlFinalize(this);
		if (m_autoLoad)
			cellSysmoduleUnloadModule(CELL_SYSMODULE_FIBER);
		return CELL_OK;
	}

	int wakeup() { return cellFiberPpuUtilWorkerControlWakeup(this); }

	int checkFlags(bool isWakingUp) { return cellFiberPpuUtilWorkerControlCheckFlags(this, isWakingUp); }

	int enablePolling(int timeout)
	{
		return cellFiberPpuUtilWorkerControlSetPollingMode(this, CELL_FIBER_PPU_UTIL_WORKER_CONTROL_POLLING_ENABLE,
		                                                   timeout);
	}

	int disablePolling()
	{
		return cellFiberPpuUtilWorkerControlSetPollingMode(this, CELL_FIBER_PPU_UTIL_WORKER_CONTROL_POLLING_DISABLE, 0);
	}

	/* Refused (BUSY) while a fiber is being created. */
	int shutdown()
	{
		if ((uintptr_t)this & 0x7f)
			return CELL_FIBER_ERROR_ALIGN;
		{
			Guard g(this);
			if (m_creating > 0)
				return CELL_FIBER_ERROR_BUSY;
			m_closing = true;
		}
		int rc = cellFiberPpuUtilWorkerControlShutdown(this);
		Guard g(this);
		if (rc == CELL_OK)
			m_shutdown = true;
		else
			m_closing = false;
		return rc;
	}

	int createFiber(CellFiberPpu *fiber, CellFiberPpuEntry entry, uint64_t arg, void *eaStack, size_t sizeStack,
	                const char *name, size_t lenName, unsigned int priority = 1,
	                CellFiberPpuOnExitCallback callback = 0, uint64_t onExitCallbackArg = 0)
	{
		if ((uintptr_t)this & 0x7f)
			return CELL_FIBER_ERROR_ALIGN;
		CellFiberPpuAttribute attr;
		cellFiberPpuAttributeInitialize(&attr);
		if (name && lenName > 0) {
			if (lenName > CELL_FIBER_PPU_NAME_MAX_LENGTH)
				return CELL_FIBER_ERROR_INVAL;
			strncpy(attr.name, name, lenName);
		}
		if (callback) {
			attr.onExitCallback = callback;
			attr.onExitCallbackArg = onExitCallbackArg;
		}
		{
			Guard g(this);
			if (m_closing)
				return CELL_FIBER_ERROR_STAT;
			m_creating++;
		}
		int rc = cellFiberPpuUtilWorkerControlCreateFiber(this, fiber, entry, arg, priority, eaStack, sizeStack, &attr);
		Guard g(this);
		m_creating--;
		return rc;
	}

	int createFiber(CellFiberPpu *fiber, CellFiberPpuEntry entry, uint64_t arg, void *eaStack, size_t sizeStack,
	                unsigned int priority = 1, CellFiberPpuOnExitCallback callback = 0,
	                uint64_t onExitCallbackArg = 0)
	{
		return createFiber(fiber, entry, arg, eaStack, sizeStack, 0, 0, priority, callback, onExitCallbackArg);
	}

	int joinFiber(CellFiberPpu *fiber, int *exitCode)
	{
		return cellFiberPpuUtilWorkerControlJoinFiber(this, fiber, exitCode);
	}

	static int sendSignal(CellFiberPpu *fiber, unsigned *numWorker = 0)
	{
		return cellFiberPpuUtilWorkerControlSendSignal(fiber, numWorker);
	}

private:
	Runtime(const Runtime &);
	Runtime &operator=(const Runtime &);

	/* m_creating / m_closing / m_shutdown change under this spin lock */
	class Guard {
	public:
		explicit Guard(Runtime *r) : m_r(r)
		{
			while (__atomic_exchange_n(&m_r->m_lock, 1u, __ATOMIC_ACQUIRE))
				;
		}
		~Guard() { __atomic_store_n(&m_r->m_lock, 0u, __ATOMIC_RELEASE); }
	private:
		Runtime *m_r;
	};

	static void worker(uint64_t arg)
	{
		cellFiberPpuUtilWorkerControlRunFibers((Runtime *)(uintptr_t)arg);
		sys_ppu_thread_exit(0);
	}

	unsigned int     m_numWorker;
	sys_ppu_thread_t m_worker[MAX_WORKER_PPU_THREAD];
	bool             m_autoLoad;
	uint32_t         m_lock;
	uint16_t         m_creating;
	bool             m_closing;
	bool             m_shutdown;
} __attribute__((aligned(CELL_FIBER_PPU_UTIL_WORKER_CONTROL_ALIGN)));

__CELL_FIBER_PPU_UTIL_END
#endif /* __cplusplus */

#endif /* __PS3DK_CELL_FIBER_PPU_UTIL_RUNTIME_H__ */

#ifdef RSX_FUNCTION_MACROS

#if RSX_UNSAFE
	#define RSX_FUNC(func)				rsx##func##Unsafe
#else
	#define RSX_FUNC(func)				rsx##func
#endif

#if RSX_INTERNAL

#if RSX_UNSAFE
	#define RSX_FUNC_INTERNAL(func)		__rsx##func##Unsafe
	#define RSX_CONTEXT_CURRENT_BEGIN(count) do {} while(0)	
#else
	s32 __attribute__((noinline)) rsxContextCallback(gcmContextData *context,u32 count)
	{
		/* The Cell target compiler calls compact [entry32, toc32] descriptors.
		 * A real call gives it ownership of argument homes and saved state,
		 * and exposes the callback's context updates to the optimizer. */
#ifdef PS3TC_PROF_AUTO
        extern void ps3dk_prof__fifo_account_check_context(void *);
        ps3dk_prof__fifo_account_check_context(context);
#endif
		return context->callback(context, count);
	}
	
	#define RSX_FUNC_INTERNAL(func)		__rsx##func
	#define RSX_CONTEXT_CURRENT_BEGIN(count) do { \
		if((context->current + (count)) > context->end) { \
			if(rsxContextCallback(context,(count))!=0) return; \
		} \
	} while(0)
#endif

#endif

#endif

#ifndef RSX_FUNCTION_MACROS
#undef RSX_CONTEXT_CURRENT_BEGIN
#undef RSX_FUNC
#undef RSX_FUNC_INTERNAL
#endif

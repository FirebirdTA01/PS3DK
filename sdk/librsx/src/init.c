#include <rsx/mm.h>
#include <rsx/gcm_sys.h>
#include <rsx/nv40.h>
#include <ppu-asm.h>
#include <sys/lv2_types.h>

gcmContextData *gGcmContext ATTRIBUTE_PRXPTR = NULL;

#define PS3TC_RSX_SUBCHANNEL_METHOD(channel, method, count) \
	(((count) << 18) | ((channel) << 13) | (method))

static gcmContextData sUserContext =
{
	NULL,
	NULL,
	NULL,
	NULL
};

extern s32 gcmInitBodyEx(gcmContextData* ATTRIBUTE_PRXPTR *ctx,const u32 cmdSize,const u32 ioSize,const void *ioAddress);

static void rsxSetInlineTransferDmaImageDestin(gcmContextData *context)
{
	if (!context || !context->current || !context->end) return;
	if (context->current >= context->end) return;

	context->current[0] = PS3TC_RSX_SUBCHANNEL_METHOD(3,NV04_CONTEXT_SURFACES_2D_DMA_IMAGE_DESTIN,1);
	context->current[1] = GCM_DMA_MEMORY_FRAME_BUFFER;
	context->current += 2;
}

s32 rsxInit(gcmContextData **context,u32 cmdSize,u32 ioSize,const void *ioAddress)
{
	s32 ret = -1;

	ret = gcmInitBodyEx(&gGcmContext,cmdSize,ioSize,ioAddress);
	if(ret==0) {
		rsxHeapInit();
		rsxSetInlineTransferDmaImageDestin(gGcmContext);

		if (context)
			*context = gGcmContext;
	}
	return ret;
}

void rsxSetupContextData(gcmContextData *context,const u32 *addr,u32 size,gcmContextCallback cb)
{
	u32 alignedSize = size&~0x3;

	context->begin = (u32*)addr;
	context->current = (u32*)addr;
	context->end = (u32*)(addr + alignedSize - 4);
	context->callback = (gcmContextCallback)(uintptr_t)lv2_fn_to_callback_ea(cb);
	rsxSetInlineTransferDmaImageDestin(context);
}

void rsxSetCurrentBuffer(gcmContextData **context,const u32 *addr,u32 size)
{
	u32 alignedSize = size&~0x3;
	
	gGcmContext = &sUserContext;

	sUserContext.begin = (u32*)addr;
	sUserContext.current = (u32*)addr;
	sUserContext.end = (u32*)((u64)addr + alignedSize - 4);
	rsxSetInlineTransferDmaImageDestin(gGcmContext);

	if (context)
		*context = gGcmContext;
}

void rsxSetDefaultCommandBuffer(gcmContextData **context)
{
	gcmSetDefaultCommandBuffer();
	rsxSetInlineTransferDmaImageDestin(gGcmContext);
	if (context)
		*context = gGcmContext;
}

void rsxSetUserCallback(gcmContextCallback cb)
{
	sUserContext.callback = cb;
}

u32* rsxGetCurrentBuffer()
{
	return gGcmContext->current;
}

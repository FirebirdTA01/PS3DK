#ifndef __RSX_INTERNAL_H__
#define __RSX_INTERNAL_H__

#include <ppc-asm.h>
#include <ppu-types.h>

#define RSX_METHOD_FLAG_NO_INCREMENT					(0x40000000)
#define RSX_METHOD_FLAG_JUMP							(0x20000000)
#define RSX_METHOD_FLAG_CALL							(0x00000002)
#define RSX_METHOD_FLAG_RETURN							(0x00020000)

#define RSX_MAX_METHOD_COUNT							0x7ff

#define RSX_CONTEXT_CURRENTP							(context->current)
#define RSX_CONTEXT_CURRENT_END(x)						context->current += (x)

#define RSX_METHOD_COUNT_SHIFT							(18)
#define RSX_METHOD(method,count)						(((count)<<RSX_METHOD_COUNT_SHIFT)|(method))
#define RSX_METHOD_NI(method,count)						(((count)<<RSX_METHOD_COUNT_SHIFT)|(method)|RSX_METHOD_FLAG_NO_INCREMENT)

#define RSX_SUBCHANNEL_SHIFT							(13)
#define RSX_SUBCHANNEL_METHOD(channel,method,count)		(((count)<<RSX_METHOD_COUNT_SHIFT)|((channel)<<RSX_SUBCHANNEL_SHIFT)|(method))

typedef struct rsx_prg
{
	u16 magic;
	u16 _pad0;

	u16 num_regs;
	u16 num_attr;
	u16 num_const;
	u16 num_insn;

	u32 attr_off;
	u32 const_off;
	u32 ucode_off;
} rsxProgram;

rsxProgramAttrib* __rsxGetAttrs(const rsxProgram *prg);
rsxProgramAttrib* __rsxGetAttr(const rsxProgram *prg, const char *name);

rsxProgramConst* __rsxGetConsts(const rsxProgram *prg);
s32 __rsxGetConstIndex(const rsxProgram *prg, const char *name);

#endif

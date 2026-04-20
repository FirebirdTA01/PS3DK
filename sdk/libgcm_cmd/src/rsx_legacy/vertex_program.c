#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <gcm_sys.h>
#include <rsx_program.h>

#include "rsx_internal.h"

void rsxVertexProgramGetUCode(const rsxVertexProgram *vp,void **ucode,u32 *size)
{
	*size = vp->num_insn*sizeof(u32)*4;
	*ucode = (void*)(((u8*)vp) + vp->ucode_off);
}

u16 rsxVertexProgramGetNumConst(const rsxVertexProgram *vp)
{
	return vp->num_const;
}

u16 rsxVertexProgramGetNumAttrib(const rsxVertexProgram *vp)
{
	return vp->num_attr;
}

rsxProgramConst* rsxVertexProgramGetConsts(const rsxVertexProgram *vp)
{
	return __rsxGetConsts((rsxProgram*) vp);
}

rsxProgramConst* rsxVertexProgramGetConst(const rsxVertexProgram *vp,const char *name)
{
	rsxProgramConst *vpc = __rsxGetConsts((rsxProgram*) vp);
	s32 index = __rsxGetConstIndex((rsxProgram*) vp, name);

	if (index == -1) return NULL;

	return &vpc[index];
}

rsxProgramAttrib* rsxVertexProgramGetAttribs(const rsxVertexProgram *vp)
{
	return __rsxGetAttrs((rsxProgram*) vp);
}

rsxProgramAttrib* rsxVertexProgramGetAttrib(const rsxVertexProgram *vp,const char *name)
{
	return __rsxGetAttr((rsxProgram*) vp, name);
}

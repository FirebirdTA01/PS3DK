#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <gcm_sys.h>
#include <rsx_program.h>

#include "rsx_internal.h"

rsxProgramAttrib* __rsxGetAttrs(const rsxProgram *prg)
{
    return (rsxProgramAttrib*)(((u8*)prg) + prg->attr_off);;
}

rsxProgramAttrib* __rsxGetAttr(const rsxProgram *prg, const char *name)
{
	u32 i;
	rsxProgramAttrib *attribs = __rsxGetAttrs(prg);
	for(i=0;i<prg->num_attr;i++) {
		char *namePtr;

		if(!attribs[i].name_off) continue;

		namePtr = ((char*)prg) + attribs[i].name_off;
		if(strcasecmp(name,namePtr)==0)
			return &attribs[i];
	}
	return NULL;
}

rsxProgramConst* __rsxGetConsts(const rsxProgram *prg)
{
    return (rsxProgramConst*)(((u8*)prg) + prg->const_off);
}

s32 __rsxGetConstIndex(const rsxProgram *prg, const char *name)
{
	u32 i;
	rsxProgramConst *consts = __rsxGetConsts(prg);
	for(i=0;i<prg->num_const;i++) {
		char *namePtr;

		if(!consts[i].name_off) continue;

		namePtr = ((char*)prg) + consts[i].name_off;
		if(strcasecmp(name,namePtr)==0)
			return i;
	}
	return -1;
}

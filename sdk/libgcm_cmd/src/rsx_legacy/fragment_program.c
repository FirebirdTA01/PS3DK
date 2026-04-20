#include <string.h>
#include <gcm_sys.h>
#include <rsx_program.h>

#include "rsx_internal.h"

void rsxFragmentProgramGetUCode(const rsxFragmentProgram *fp,void **ucode,u32 *size)
{
	*size = fp->num_insn*sizeof(u32)*4;
	*ucode = (void*)(((u8*)fp) + fp->ucode_off);
}

u16 rsxFragmentProgramGetNumConst(const rsxFragmentProgram *fp)
{
	return fp->num_const;
}

rsxProgramConst* rsxFragmentProgramGetConsts(const rsxFragmentProgram *fp)
{
	return __rsxGetConsts((rsxProgram*) fp);
}

rsxProgramConst* rsxFragmentProgramGetConst(const rsxFragmentProgram *fp,const char *name)
{
	rsxProgramConst *fpc = __rsxGetConsts((rsxProgram*) fp);
	s32 index = __rsxGetConstIndex((rsxProgram*) fp, name);

	if (index == -1) return NULL;

	return &fpc[index];
}

u16 rsxFragmentProgramGetNumAttrib(const rsxFragmentProgram *fp)
{
	return fp->num_attr;
}

rsxProgramAttrib* rsxFragmentProgramGetAttribs(const rsxFragmentProgram *fp)
{
	return __rsxGetAttrs((rsxProgram*) fp);
}

rsxProgramAttrib* rsxFragmentProgramGetAttrib(const rsxFragmentProgram *fp,const char *name)
{
	return __rsxGetAttr((rsxProgram*) fp, name);
}

rsxConstOffsetTable* rsxFragmentProgramGetConstOffsetTable(const rsxFragmentProgram *fp,u32 table_off)
{
	return (rsxConstOffsetTable*)(((u8*)fp) + table_off);
}

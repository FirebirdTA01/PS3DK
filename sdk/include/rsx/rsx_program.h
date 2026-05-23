/*! \file rsx_program.h
\brief RSX shader programming management.
*/

#ifndef __RSX_PROGRAM_H__
#define __RSX_PROGRAM_H__

#include <ppu-types.h>

#define PARAM_BOOL				0
#define PARAM_BOOL1				1
#define PARAM_BOOL2				2
#define PARAM_BOOL3				3
#define PARAM_BOOL4				4
#define PARAM_FLOAT				5
#define PARAM_FLOAT1			6
#define PARAM_FLOAT2			7
#define PARAM_FLOAT3			8
#define PARAM_FLOAT4			9
#define PARAM_FLOAT3x4			10
#define PARAM_FLOAT4x4			11
#define PARAM_FLOAT3x3			12
#define PARAM_FLOAT4x3			13
#define PARAM_SAMPLER1D			14
#define PARAM_SAMPLER2D			15
#define PARAM_SAMPLER3D			16
#define PARAM_SAMPLERCUBE		17
#define PARAM_SAMPLERRECT		18
#define PARAM_UNKNOWN			0xff

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief Vertex program data structure.

This data structure is filled by cgcomp, the offline compiler for shader programs. */
typedef struct rsx_vp
{
	u16 magic;			/*!< \brief magic identifier */
	u16 _pad0;			/*!< \brief unused padding word. most be 0 */

	u16 num_regs;		/*!< \brief number of used registers in the vertex program */
	u16 num_attr;		/*!< \brief number of used input attributes in the vertex program */
	u16 num_const;		/*!< \brief number of used constants in the vertex program */
	u16 num_insn;		/*!< \brief number of vertex shader instructions */

	u32 attr_off;		/*!< \brief offset to the attribute name table */
	u32 const_off;		/*!< \brief offset to the constant name table */
	u32 ucode_off;		/*!< \brief offset to the shader's micro code */

	u32 input_mask;		/*!< \brief mask of input attributes in the vertex shader */
	u32 output_mask;	/*!< \brief mask of result attributes passed to the fragment shader */

	u16 const_start;	/*!< \brief start address in vertex shader's constant block memory */
	u16 insn_start;		/*!< \brief start address to load the vertex program to */
} rsxVertexProgram;

/*! \brief Fragment program data structure.

This data structure is filled by cgcomp, the offline compiler for shader programs. */
typedef struct rsx_fp
{
	u16 magic;			/*!< \brief magic identifier */ 
	u16 _pad0;			/*!< \brief unused padding word. most be 0 */

	u16 num_regs;		/*!< \brief number of used registers in the fragment program */
	u16 num_attr;		/*!< \brief number of used input attributes in the fragment program */
	u16 num_const;		/*!< \brief number of used constants in the fragment program */
	u16 num_insn;		/*!< \brief number of fragment program instructions */

	u32 attr_off;		/*!< \brief offset to the attribute name table */
	u32 const_off;		/*!< \brief offset to the constant name table */
	u32 ucode_off;		/*!< \brief offset to the shaders's micro code */

	u32 fp_control;		/*!< \brief fragment program control mask */

	u16 texcoords;		/*!< \brief bit mask of all used texture coords in the fragment program */
	u16 texcoord2D;		/*!< \brief bit mask of used 2D texture coords in the fragment program */
	u16 texcoord3D;		/*!< \brief bit mask of used 3D texture coords in the fragment program */
	u16 _pad1;			/*!< \brief unused padding word. most be 0 */
} rsxFragmentProgram;

/*! \brief Program const data structure. */
typedef struct rsx_const
{
	u32 name_off;		/*!< \brief offset of name. */
	u32 index;			/*!< \brief program const id. */
	u8 type;			/*!< \brief program const type. */
	u8 is_internal;		/*!< \brief internal flag. */
	u8 count;			/*!< \brief number of elements in the const. */

	u8 _pad0;			/*!< \brief unused padding byte, most be 0. */

	union {
		u32 u;			/*!< \brief integer value. */
		f32 f;			/*!< \brief float value. */
	} values[4];		/*!< \brief array of element values.
	
	  Number of elements is in the \p count field.
	*/


} rsxProgramConst;

/*! \brief Table of const offsets. */
typedef struct rsx_co_table
{
	u32 num;		/*!< \brief number of elements in the array. */
	u32 offset[];	/*!< \brief array of const offsets. */
} rsxConstOffsetTable;

/*! \brief Table of program attributes. */
typedef struct rsx_attrib
{
	u32 name_off;	/*!< \brief offset of attribute name. */
	u32 index;		/*!< \brief attribute index. */
	u8 type;		/*!< \brief attribute type. */
	u8 _pad0[3];
} rsxProgramAttrib;

/*! \brief Get Ucode from RSX vertex program.
\param vp Pointer the to vertex program structure.
\param ucode Pointer-pointer to receive the vertex program ucode.
\param size Pointer to receive the vertex program ucode size.
*/
void rsxVertexProgramGetUCode(const rsxVertexProgram *vp,void **ucode,u32 *size);

/*! \brief Get the count of vertex program consts.
\param vp Pointer the to vertex program structure.
\return Count of constants in the vertex program.
*/
u16 rsxVertexProgramGetNumConst(const rsxVertexProgram *vp);

/*! \brief Get the list of vertex program consts.
\param vp Pointer the to vertex program structure.
\return Pointer to the list of program const structures.
*/
rsxProgramConst* rsxVertexProgramGetConsts(const rsxVertexProgram *vp);

/*! \brief Get index of vertex program const from its name.
\param vp Pointer the to vertex program structure.
\param name Name of the vertex program const.
\return The requested vertex program const id.
*/
s32 rsxVertexProgramGetConstIndex(const rsxVertexProgram *vp,const char *name);

/*! \brief Get const value of vertex program const from its name.
\param vp Pointer the to vertex program structure.
\param name Name of the vertex program const.
\return The requested vertex program const value.
*/
rsxProgramConst* rsxVertexProgramGetConst(const rsxVertexProgram *vp,const char *name);

/*! \brief Get the count of vertex program attributes.
\param vp Pointer the to vertex program structure.
\return Count of attributes in the vertex program.
*/
u16 rsxVertexProgramGetNumAttrib(const rsxVertexProgram *vp);

/*! \brief Get the list of vertex program attributes.
\param vp Pointer the to vertex program structure.
\return Pointer to the list of program attribute structures.
*/
rsxProgramAttrib* rsxVertexProgramGetAttribs(const rsxVertexProgram *vp);

/*! \brief Get attribute value of vertex program attribute from its name.
\param vp Pointer the to vertex program structure.
\param name Name of the vertex program attribute.
\return The requested vertex program attribute value.
*/
rsxProgramAttrib* rsxVertexProgramGetAttrib(const rsxVertexProgram *vp,const char *name);

/*! \brief Get index of vertex program attribute from its name.
\param vp Pointer the to vertex program structure.
\param name Name of the vertex program attribute.
\return The requested vertex program attribute index.
*/
s32 rsxVertexProgramGetAttribIndex(const rsxVertexProgram *vp,const char *name);

/*! \brief Get Ucode from RSX fragment program.
\param fp Pointer the to fragment program structure.
\param ucode Pointer-pointer to receive the fragment program ucode.
\param size Pointer to receive the fragment program ucode size.
*/
void rsxFragmentProgramGetUCode(const rsxFragmentProgram *fp,void **ucode,u32 *size);

/*! \brief Get the count of fragment program consts.
\param fp Pointer the to fragment program structure.
\return Count of constants in the fragment program.
*/
u16 rsxFragmentProgramGetNumConst(const rsxFragmentProgram *fp);

/*! \brief Get the list of fragment program consts.
\param fp Pointer the to fragment program structure.
\return Pointer to the list of program const structures.
*/
rsxProgramConst* rsxFragmentProgramGetConsts(const rsxFragmentProgram *fp);

/*! \brief Get index of fragment program const from its name.
\param fp Pointer the to fragment program structure.
\param name Name of the fragment program const.
\return The requested fragment program const index.
*/
s32 rsxFragmentProgramGetConstIndex(const rsxFragmentProgram *fp,const char *name);

/*! \brief Get const value of fragment program const from its name.
\param fp Pointer the to fragment program structure.
\param name Name of the fragment program const.
\return The requested fragment program const value.
*/
rsxProgramConst* rsxFragmentProgramGetConst(const rsxFragmentProgram *fp,const char *name);

/*! \brief Get the count of fragment program attributes.
\param fp Pointer the to fragment program structure.
\return Count of attributes in the fragment program.
*/
u16 rsxFragmentProgramGetNumAttrib(const rsxFragmentProgram *fp);

/*! \brief Get the list of fragment program attributes.
\param fp Pointer the to fragment program structure.
\return Pointer to the list of program attribute structures.
*/
rsxProgramAttrib* rsxFragmentProgramGetAttribs(const rsxFragmentProgram *fp);

/*! \brief Get index of fragment program attribute from its name.
\param fp Pointer the to fragment program structure.
\param name Name of the fragment program attribute.
\return The requested fragment program attribute index.
*/
s32 rsxFragmentProgramGetAttribIndex(const rsxFragmentProgram *fp,const char *name);

/*! \brief Get attribute value of fragment program attribute from its name.
\param fp Pointer the to fragment program structure.
\param name Name of the fragment program attribute.
\return The requested fragment program attribute value.
*/
rsxProgramAttrib* rsxFragmentProgramGetAttrib(const rsxFragmentProgram *fp,const char *name);

/*! \brief Get const offset table from a fragment program.
\param fp Pointer the to fragment program structure.
\param table_off Offset of the const offset table.
\return Pointer to the requested const offset table.
*/
rsxConstOffsetTable* rsxFragmentProgramGetConstOffsetTable(const rsxFragmentProgram *fp,u32 table_off);

#ifdef __cplusplus
	}
#endif

#endif

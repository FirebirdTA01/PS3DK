/* Frozen verbatim emitter bodies from 60fe5924092ccbdc3f61e681fd558d056146941a.
 * commands_impl.h LF SHA256: 9c522540894a0521ee52aac209ed71ea19bdc6820605d4e43deea6b9b530dcce
 * This fixture is never linked into SDK libraries.

Copyright (c) 2011 PSL1GHT Development Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawVertexArray)(gcmContextData *context,u32 type,u32 start,u32 count)
{
	u32 i,j,lcount,loop,rest;
	
	--count;
	lcount = count&0xff;
	count >>= 8;
	
	loop = count/RSX_MAX_METHOD_COUNT;
	rest = count%RSX_MAX_METHOD_COUNT;
	
	RSX_CONTEXT_CURRENT_BEGIN(8);

	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD_NI(NV40TCL_VTX_CACHE_INVALIDATE,3);
	RSX_CONTEXT_CURRENTP[1] = 0;
	RSX_CONTEXT_CURRENTP[2] = 0;
	RSX_CONTEXT_CURRENTP[3] = 0;
	
	RSX_CONTEXT_CURRENTP[4] = RSX_METHOD(NV40TCL_BEGIN_END,1);
	RSX_CONTEXT_CURRENTP[5] = type;

	RSX_CONTEXT_CURRENTP[6] = RSX_METHOD(NV40TCL_VB_VERTEX_BATCH,1);
	RSX_CONTEXT_CURRENTP[7] = ((lcount<<24) | start);
	
	RSX_CONTEXT_CURRENT_END(8);
	
	start += (lcount + 1);
	for(i=0;i < loop;i++) {
		RSX_CONTEXT_CURRENT_BEGIN(1 + RSX_MAX_METHOD_COUNT);
		
		RSX_CONTEXT_CURRENTP[0] = RSX_METHOD_NI(NV40TCL_VB_VERTEX_BATCH, RSX_MAX_METHOD_COUNT);
		RSX_CONTEXT_CURRENTP++;
		
		for(j=0;j<RSX_MAX_METHOD_COUNT;j++) {
			RSX_CONTEXT_CURRENTP[0] = (0xff000000 | start);
			RSX_CONTEXT_CURRENTP++;
			start += 0x100;
		}
		
	}	

	if(rest) {
		RSX_CONTEXT_CURRENT_BEGIN(1 + rest);

		RSX_CONTEXT_CURRENTP[0] = RSX_METHOD_NI(NV40TCL_VB_VERTEX_BATCH, rest);
		RSX_CONTEXT_CURRENTP++;
		
		for(j=0;j < rest;j++) {
			RSX_CONTEXT_CURRENTP[0] = (0xff000000 | start);
			RSX_CONTEXT_CURRENTP++;
			start += 0x100;
		}
	}
		
	RSX_CONTEXT_CURRENT_BEGIN(2);
	
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_BEGIN_END, 1);
	RSX_CONTEXT_CURRENTP[1] = NV40TCL_BEGIN_END_STOP;

	RSX_CONTEXT_CURRENT_END(2);
}

static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawIndexArray)(gcmContextData *context,u8 type,u32 offset,u32 count,u8 data_type,u8 location)
{
	u32 i,start,mcount;
	u32 misalignedcount,odd;

	if(data_type == GCM_INDEX_TYPE_32B)
		misalignedcount = (((offset + 127)&~127) - offset)>>2;
	else
		misalignedcount = (((offset + 127)&~127) - offset)>>1;

	odd = (misalignedcount && misalignedcount < count) ? 1 : 0;
	
	RSX_CONTEXT_CURRENT_BEGIN(7 + odd*2);

	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
	RSX_CONTEXT_CURRENTP[1] = 0;

	RSX_CONTEXT_CURRENTP[2] = RSX_METHOD(NV40TCL_VB_INDEX_BATCH_OFFSET, 2);
	RSX_CONTEXT_CURRENTP[3] = offset;
	RSX_CONTEXT_CURRENTP[4] = (((data_type) << 4) | location);

	RSX_CONTEXT_CURRENTP[5] = RSX_METHOD(NV40TCL_BEGIN_END, 1);
	RSX_CONTEXT_CURRENTP[6] = type;
	
	start = 0;
	if(odd) {
		u32 tmp = misalignedcount - 1;
		
		RSX_CONTEXT_CURRENTP[7] = RSX_METHOD(NV40TCL_VB_INDEX_BATCH_DRAW, 1);
		RSX_CONTEXT_CURRENTP[8] = ((tmp<<24) | start);

		start += misalignedcount;
		count -= misalignedcount;
	}
	
	RSX_CONTEXT_CURRENT_END(7 + odd*2);

	while(count > 0x7ff00) {
		RSX_CONTEXT_CURRENT_BEGIN(1 + RSX_MAX_METHOD_COUNT);
		
		RSX_CONTEXT_CURRENTP[0] = RSX_METHOD_NI(NV40TCL_VB_INDEX_BATCH_DRAW, RSX_MAX_METHOD_COUNT);
		RSX_CONTEXT_CURRENTP++;
		
		for(i=0;i < RSX_MAX_METHOD_COUNT;i++) {
			RSX_CONTEXT_CURRENTP[0] = (0xff000000 | start);
			RSX_CONTEXT_CURRENTP++;
			start += 0x100;
		}

		count -= 0x7ff00;
	}
	
	mcount = (count + 0xff)>>8;
	
	RSX_CONTEXT_CURRENT_BEGIN(1 + mcount);
	
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD_NI(NV40TCL_VB_INDEX_BATCH_DRAW, mcount);
	RSX_CONTEXT_CURRENTP++;
	
	while(count > 0x100) {
		RSX_CONTEXT_CURRENTP[0] = (0xff000000 | start);
		RSX_CONTEXT_CURRENTP++;
		start += 0x100;
		count -= 0x100;
	}
	
	if(count) {
		--count;	
		RSX_CONTEXT_CURRENTP[0] = ((count<<24) | start);
		RSX_CONTEXT_CURRENTP++;
	}	

	RSX_CONTEXT_CURRENT_BEGIN(2);
	
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_BEGIN_END, 1);
	RSX_CONTEXT_CURRENTP[1] = NV40TCL_BEGIN_END_STOP;
 	
	RSX_CONTEXT_CURRENT_END(2);
}

void RSX_FUNC(ClearSurface)(gcmContextData *context,u32 clear_mask)
{
	RSX_CONTEXT_CURRENT_BEGIN(6);

	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_CLEAR_BUFFERS,1);
	RSX_CONTEXT_CURRENTP[1] = clear_mask;
	RSX_CONTEXT_CURRENTP[2] = RSX_METHOD(NV40TCL_NOP,1);
	RSX_CONTEXT_CURRENTP[3] = 0;
	RSX_CONTEXT_CURRENTP[4] = RSX_METHOD(NV40TCL_WAIT_FOR_IDLE,1);
	RSX_CONTEXT_CURRENTP[5] = 0;

	RSX_CONTEXT_CURRENT_END(6);
}

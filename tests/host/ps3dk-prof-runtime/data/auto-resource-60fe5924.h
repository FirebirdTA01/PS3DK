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

void RSX_FUNC(SetWriteBackendLabel)(gcmContextData *context,u8 index,u32 value)
{
	u32 offset = 0x10*index;
	u32 wvalue = (value&0xff00ff00) | ((value>>16)&0xff) | ((value&0xff)<<16);

	RSX_CONTEXT_CURRENT_BEGIN(4);
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_SEMAPHORE_OFFSET,1);
	RSX_CONTEXT_CURRENTP[1] = offset;
	RSX_CONTEXT_CURRENTP[2] = RSX_METHOD(NV40TCL_SEMAPHORE_BACKENDWRITE_RELEASE,1);
	RSX_CONTEXT_CURRENTP[3] = wvalue;
	RSX_CONTEXT_CURRENT_END(4);
}

void RSX_FUNC(SetWriteTextureLabel)(gcmContextData *context,u8 index,u32 value)
{
	u32 offset = 0x10*index;

	RSX_CONTEXT_CURRENT_BEGIN(4);
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_SEMAPHORE_OFFSET,1);
	RSX_CONTEXT_CURRENTP[1] = offset;
	RSX_CONTEXT_CURRENTP[2] = RSX_METHOD(NV40TCL_SEMAPHORE_TEXTUREREAD_RELEASE,1);
	RSX_CONTEXT_CURRENTP[3] = value;
	RSX_CONTEXT_CURRENT_END(4);
}

void RSX_FUNC(SetWriteCommandLabel)(gcmContextData *context,u8 index,u32 value)
{
	u32 offset = 0x10*index;

	RSX_CONTEXT_CURRENT_BEGIN(4);
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV406ETCL_SEMAPHORE_OFFSET,1);
	RSX_CONTEXT_CURRENTP[1] = offset;
	RSX_CONTEXT_CURRENTP[2] = RSX_METHOD(NV406ETCL_SEMAPHORE_RELEASE,1);
	RSX_CONTEXT_CURRENTP[3] = value;
	RSX_CONTEXT_CURRENT_END(4);
}

void RSX_FUNC(SetReport)(gcmContextData *context,u32 type,u32 index)
{
	u32 offset = 0x10*index;

	RSX_CONTEXT_CURRENT_BEGIN(2);
	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_QUERY_GET,1);
	RSX_CONTEXT_CURRENTP[1] = ((type<<24) | offset);
	RSX_CONTEXT_CURRENT_END(2);
}

void RSX_FUNC(SetTimeStamp)(gcmContextData *context,u32 index)
{
	RSX_CONTEXT_CURRENT_BEGIN(2);

	RSX_CONTEXT_CURRENTP[0] = RSX_METHOD(NV40TCL_QUERY_GET,1);
	RSX_CONTEXT_CURRENTP[1] = (((index << 4)&0x0fffffff) | 0x10000000);

	RSX_CONTEXT_CURRENT_END(2);
}

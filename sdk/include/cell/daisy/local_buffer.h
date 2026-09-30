/* cell/daisy/local_buffer.h - Buffer::Local: the entries live in memory the
 * calling core addresses directly (local store on the SPU, main memory on
 * the PPU) and are moved with memcpy, or handed out as references
 * (REFERENCE ports).
 *
 * tType's size must be a multiple of 16 bytes.  The PPU form can also wrap
 * a caller-allocated array (16-byte aligned, at least tSize entries). */
#ifndef PS3TC_CELL_DAISY_LOCAL_BUFFER_H
#define PS3TC_CELL_DAISY_LOCAL_BUFFER_H

#include <cell/daisy/buffer.h>

namespace cell {
namespace Daisy {
namespace Buffer {

#ifdef __SPU__
template <typename tType, SizeType tSize, ConstructorMode tConstructorMode = NO_PARAMETER>
#else
template <typename tType, SizeType tSize>
#endif
class Local : public Abstract<tType, tSize> {
public:
	static const BufferType sBufferType = BUFFER_TYPE_LOCAL;

	explicit Local() : mEntries(mStorage) {}
#ifndef __SPU__
	explicit Local(volatile tType *pBuffer) : mEntries(pBuffer) {}
#endif

	volatile tType *getEntryReference(PointerType pointer)
	{
		return &mEntries[pointer % tSize];
	}
	void copyIn(PointerType pointer, const tType *data)
	{
		memcpy((void *)getEntryReference(pointer), (const void *)data, sizeof(tType));
	}
	void copyOut(PointerType pointer, tType *data)
	{
		memcpy((void *)data, (const void *)getEntryReference(pointer), sizeof(tType));
	}
	const char *getClassName() { return "Buffer::Local"; }

private:
	typedef char sSizeCheck[(sizeof(tType) % 16 == 0) ? 1 : -1];
	volatile tType *mEntries;
	volatile tType mStorage[tSize] __attribute__((aligned(128)));
};

} /* namespace Buffer */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_LOCAL_BUFFER_H */

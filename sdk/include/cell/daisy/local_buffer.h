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
#ifdef __SPU__
#include <spu_mfcio.h>
#endif

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
#ifdef __SPU__
	/* PARAMETER: publish the entries' effective address (this is SPU
	 * spuNum of the group) at parameterEa + CELL_DAISY_BUFFER_PARAM_OFFSET
	 * for the Buffer::Remote at the other end */
	explicit Local(uint64_t parameterEa, int spuNum) : mEntries(mStorage)
	{
		uint64_t ea = CELL_DAISY_GET_LS_AREA(spuNum) + (uint32_t)(uintptr_t)mStorage;
		mParam[0] = (uint32_t)(ea >> 32);
		mParam[1] = (uint32_t)ea;
		mParam[2] = 1;
		mParam[3] = 0;
		mfc_put(mParam, parameterEa + CELL_DAISY_BUFFER_PARAM_OFFSET, 16, 31, 0, 0);
		mfc_write_tag_mask(1u << 31);
		mfc_read_tag_status_all();
	}
#else
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
	/* nothing is in flight: copies finish in copyIn / copyOut */
	bool transferDone(PointerType) { return true; }
	void waitTransfer(PointerType) {}
	const char *getClassName() { return "Buffer::Local"; }

private:
	typedef char sSizeCheck[(sizeof(tType) % 16 == 0) ? 1 : -1];
	volatile tType *mEntries;
	volatile tType mStorage[tSize] __attribute__((aligned(128)));
#ifdef __SPU__
	volatile uint32_t mParam[4] __attribute__((aligned(16)));
#endif
};

} /* namespace Buffer */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_LOCAL_BUFFER_H */

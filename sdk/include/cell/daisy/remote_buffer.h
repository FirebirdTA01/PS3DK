/* cell/daisy/remote_buffer.h - Buffer::Remote (SPU): the entries live in main
 * memory at bufferStartEa and move by DMA.  Entry n uses tag
 * (dmaTag + ((n % tSize) & dmaTagRangeMask)) % 32, so up to tSize transfers
 * overlap; a port waits for an entry's tag before handing it over.
 * sizeof(tType) is a multiple of 16 bytes, at most 16 KB. */
#ifndef PS3TC_CELL_DAISY_REMOTE_BUFFER_H
#define PS3TC_CELL_DAISY_REMOTE_BUFFER_H

#include <cell/daisy/buffer.h>
#include <spu_mfcio.h>

namespace cell {
namespace Daisy {
namespace Buffer {

template <typename tType, SizeType tSize, ConstructorMode tConstructorMode = NO_PARAMETER>
class Remote : public Abstract<tType, tSize> {
public:
	static const BufferType sBufferType = BUFFER_TYPE_REMOTE;

	explicit Remote(uint64_t bufferStartEa, uint32_t dmaTag, uint32_t dmaTagRangeMask = 0xffffffff)
		: mEa(bufferStartEa), mTag(dmaTag), mMask(dmaTagRangeMask)
	{
	}

	void setDmaTagRangeMask(uint32_t dmaTagRangeMask) { mMask = dmaTagRangeMask; }
	uint32_t getTag() const { return mTag; }

	void copyIn(PointerType pointer, const tType *data)
	{
		mfc_put((volatile void *)data, entryEa(pointer), sizeof(tType), tagOf(pointer), 0, 0);
	}
	void copyOut(PointerType pointer, tType *data)
	{
		mfc_get((volatile void *)data, entryEa(pointer), sizeof(tType), tagOf(pointer), 0, 0);
	}
	bool transferDone(PointerType pointer)
	{
		mfc_write_tag_mask(1u << tagOf(pointer));
		return mfc_read_tag_status_immediate() != 0;
	}
	void waitTransfer(PointerType pointer)
	{
		mfc_write_tag_mask(1u << tagOf(pointer));
		mfc_read_tag_status_all();
	}
	const char *getClassName() { return "Buffer::Remote"; }

private:
	typedef char sSizeCheck[(sizeof(tType) % 16 == 0 && sizeof(tType) <= 16384) ? 1 : -1];
	uint64_t entryEa(PointerType pointer) const { return mEa + (uint64_t)(pointer % tSize) * sizeof(tType); }
	uint32_t tagOf(PointerType pointer) const { return (mTag + ((pointer % tSize) & mMask)) % 32; }

	uint64_t mEa;
	uint32_t mTag, mMask;
};

} /* namespace Buffer */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_REMOTE_BUFFER_H */

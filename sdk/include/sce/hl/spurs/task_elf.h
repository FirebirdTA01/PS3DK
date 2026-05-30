#ifndef __PS3DK_SCE_HL_SPURS_TASK_ELF_H__
#define __PS3DK_SCE_HL_SPURS_TASK_ELF_H__

#include <stdint.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class TaskElf {
    uint64_t m_upper;
    uint64_t m_lower;
    const void *m_elf;

public:
    explicit TaskElf(const void *elf) : m_upper(0), m_lower(0), m_elf(elf) {}

    const void *elf() const { return m_elf; }
    const CellSpursTaskLsPattern *lsPattern() const
    { return reinterpret_cast<const CellSpursTaskLsPattern *>(this); }

    unsigned saveBufferSize() const
    {
        return static_cast<unsigned>(__builtin_popcountll(m_upper) +
                                     __builtin_popcountll(m_lower)) * 2048u +
               CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE;
    }

    static unsigned saveBufferAlign() { return CELL_SPURS_TASK_CONTEXT_ALIGN; }

    void setRange(unsigned start, unsigned size) { setRangeBits(start, size, true); }
    void unsetRange(unsigned start, unsigned size) { setRangeBits(start, size, false); }
    void unsetReadOnlySegment() {}
    void setWritableSegment() {}
    void setStack(unsigned stackSize)
    { setRange(CELL_SPURS_TASK_BOTTOM - stackSize, stackSize); }
    void unsetAll() { m_upper = 0; m_lower = 0; }
    void setAll() { m_upper = ~0ULL; m_lower = ~0ULL; }

private:
    void setRangeBits(unsigned start, unsigned size, bool set)
    {
        unsigned first = start / 2048u;
        unsigned last = (start + size + 2047u) / 2048u;
        if (last > 128u) last = 128u;
        for (unsigned bit = first; bit < last; ++bit) {
            uint64_t mask = 1ULL << (bit & 63u);
            if (bit < 64u) {
                m_upper = set ? (m_upper | mask) : (m_upper & ~mask);
            } else {
                m_lower = set ? (m_lower | mask) : (m_lower & ~mask);
            }
        }
    }
};

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_TASK_ELF_H__ */

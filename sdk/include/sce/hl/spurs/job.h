#ifndef __PS3DK_SCE_HL_SPURS_JOB_H__
#define __PS3DK_SCE_HL_SPURS_JOB_H__

#include <stdint.h>
#include <string.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/event_flag.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

namespace Job {

class Descriptor {
protected:
    CellSpursJobHeader mHeader;

public:
    Descriptor() { __builtin_memset(&mHeader, 0, sizeof(mHeader)); }
    void setBinary(const void *address, const void *size)
    { setBinary(address, static_cast<unsigned>(reinterpret_cast<uintptr_t>(size))); }
    void setBinary(const void *address, unsigned size)
    {
        mHeader.eaBinary = reinterpret_cast<uintptr_t>(address);
        mHeader.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(size);
    }
};

class Command {
    uint64_t m_command;

public:
    Command() : m_command(CELL_SPURS_JOB_COMMAND_NOP) {}
    explicit Command(uint64_t command) : m_command(command) {}
    operator uint64_t() const { return m_command; }
};

class Nop : public Command { public: Nop() : Command(CELL_SPURS_JOB_COMMAND_NOP) {} };
class Job : public Command {
public:
    explicit Job(Descriptor &desc) : Command(CELL_SPURS_JOB_COMMAND_JOB(&desc)) {}
    explicit Job(Descriptor *desc) : Command(CELL_SPURS_JOB_COMMAND_JOB(desc)) {}
};
class JobList : public Command {
public:
    explicit JobList(CellSpursJobList &list) : Command(CELL_SPURS_JOB_COMMAND_JOBLIST(&list)) {}
    explicit JobList(CellSpursJobList *list) : Command(CELL_SPURS_JOB_COMMAND_JOBLIST(list)) {}
};
class Guard : public Command {
public:
    explicit Guard(CellSpursJobGuard &guard) : Command(CELL_SPURS_JOB_COMMAND_GUARD(&guard)) {}
    explicit Guard(CellSpursJobGuard *guard) : Command(CELL_SPURS_JOB_COMMAND_GUARD(guard)) {}
};
class Sync : public Command { public: Sync() : Command(CELL_SPURS_JOB_COMMAND_SYNC) {} };
class LWSync : public Command { public: LWSync() : Command(CELL_SPURS_JOB_COMMAND_LWSYNC) {} };
class SetLabel : public Command {
public:
    explicit SetLabel(unsigned label = 0) : Command(CELL_SPURS_JOB_COMMAND_SET_LABEL(label)) {}
};
class SyncLabel : public Command {
public:
    explicit SyncLabel(unsigned label = 0) : Command(CELL_SPURS_JOB_COMMAND_SYNC_LABEL(label)) {}
};
class LWSyncLabel : public Command {
public:
    explicit LWSyncLabel(unsigned label = 0) : Command(CELL_SPURS_JOB_COMMAND_LWSYNC_LABEL(label)) {}
};
class ResetPC : public Command {
public:
    explicit ResetPC(const uint64_t *command) : Command(CELL_SPURS_JOB_COMMAND_RESET_PC(command)) {}
    explicit ResetPC(const Command *command) : Command(CELL_SPURS_JOB_COMMAND_RESET_PC(command)) {}
};
class Next : public Command {
public:
    explicit Next(const uint64_t *command) : Command(CELL_SPURS_JOB_COMMAND_NEXT(command)) {}
    explicit Next(const Command *command) : Command(CELL_SPURS_JOB_COMMAND_NEXT(command)) {}
};
class Call : public Command {
public:
    explicit Call(const uint64_t *command) : Command(CELL_SPURS_JOB_COMMAND_CALL(command)) {}
    explicit Call(const Command *command) : Command(CELL_SPURS_JOB_COMMAND_CALL(command)) {}
};
class Ret : public Command { public: Ret() : Command(CELL_SPURS_JOB_COMMAND_RET) {} };
class Abort : public Command { public: Abort() : Command(CELL_SPURS_JOB_COMMAND_ABORT) {} };
class End : public Command { public: End() : Command(CELL_SPURS_JOB_COMMAND_END) {} };
class Flush : public Command { public: Flush() : Command(CELL_SPURS_JOB_COMMAND_FLUSH) {} };

class Notification : public Descriptor {
    union {
        uint32_t mEaEventFlag;
        EventFlag *mEventFlag;
    };

public:
    Notification() : mEventFlag(0) {}
    explicit Notification(EventFlag *eventFlag) : mEventFlag(0)
    { initialize(this, eventFlag); }

    static int initialize(Notification *self, EventFlag *eventFlag)
    {
        self->mEventFlag = eventFlag;
        return CELL_OK;
    }

    int tryWait()
    {
        uint16_t bits = 1;
        return mEventFlag ? mEventFlag->tryWait(&bits, CELL_SPURS_EVENT_FLAG_OR)
                          : CELL_SPURS_JOB_ERROR_INVAL;
    }

    int wait()
    {
        uint16_t bits = 1;
        if (!mEventFlag) return CELL_SPURS_JOB_ERROR_INVAL;
        int ret = mEventFlag->tryWait(&bits, CELL_SPURS_EVENT_FLAG_OR);
        if (ret == CELL_OK) return CELL_OK;
        __SCE_SPURS_UTIL_RETURN_IF(mEventFlag->attachLv2EventQueue());
        ret = mEventFlag->wait(&bits, CELL_SPURS_EVENT_FLAG_OR);
        mEventFlag->detachLv2EventQueue();
        return ret;
    }
};

} /* namespace Job */

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_JOB_H__ */

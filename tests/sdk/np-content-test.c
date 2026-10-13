/* The NP names added to <cell/np/common.h> and <cell/np/tus.h> and the
 * <np/common.h>, <np/ticket.h>, <np/tustorage.h> paths: language codes,
 * sizes, the ticket subject accessors and the TSS context aliases.
 * np-content-test.sh compiles this in C and C++, ILP32 and LP64, and links
 * the TSS aliases against the TUS stub archive when an SDK is installed. */
#include <stdint.h>
#include <np/common.h>
#include <np/ticket.h>
#include <np/tustorage.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif

STATIC_ASSERT(SCE_NP_LANG_JAPANESE == 0 && SCE_NP_LANG_FRENCH_CA == 22, "language range");
STATIC_ASSERT(SCE_NP_LANG_ENGLISH == SCE_NP_LANG_ENGLISH_US && SCE_NP_LANG_PORTUGUESE == SCE_NP_LANG_PORTUGUESE_PT,
              "older language names");
STATIC_ASSERT(SCE_NP_FRIEND_MAX_NUM == 100 && SCE_NP_PORT == 3658, "friend list and port");
STATIC_ASSERT(SCE_NP_TSS_MAX_SIZE == 65536u && SCE_NET_NP_TSS_MAX_SIZE == SCE_NP_TSS_MAX_SIZE, "tss size");
STATIC_ASSERT(SCE_NET_NP_PSHANDLE_MAX_LENGTH == SCE_NET_NP_ONLINEID_MAX_LENGTH, "handle length alias");
STATIC_ASSERT(sizeof(SceNpUserIcon) == sizeof(SceNpAvatarUrl), "user icon is the avatar url");
/* Status word: age in bits 24-30, then three flags. */
STATIC_ASSERT(SCE_NP_SUBJECT_STATUS_GET_AGE(0x2a000000u) == 42, "age");
STATIC_ASSERT(SCE_NP_SUBJECT_STATUS_GET_AGE(0xffffffffu) == 0x7f, "age is seven bits");
STATIC_ASSERT(SCE_NP_SUBJECT_STATUS_IS_SUSPENDED(0x80u) && !SCE_NP_SUBJECT_STATUS_IS_SUSPENDED(0x100u), "suspended");
STATIC_ASSERT(SCE_NP_SUBJECT_STATUS_IS_CHAT_DISABLED(0x100u) && SCE_NP_SUBJECT_STATUS_CONTENT_RATING(0x200u), "flags");

/* The statement accessors, under an unbraced if (they are single statements). */
int subject_fields(const char *raw_id, const char *raw_region, int take, SceNpOnlineId *id, SceNpCountryCode *cc);
int subject_fields(const char *raw_id, const char *raw_region, int take, SceNpOnlineId *id, SceNpCountryCode *cc)
{
	if (take)
		SCE_NP_SUBJECT_ONLINE_ID_GET_ONLINE_ID(raw_id, id);
	else
		SCE_NP_SUBJECT_REGION_GET_COUNTRY_CODE(raw_region, cc);
	return SCE_NP_SUBJECT_REGION_GET_LANGUAGE_CODE(raw_region);
}

/* The TSS names are the TUS context calls. */
int tss_contexts(const SceNpCommunicationId *c, const SceNpCommunicationPassphrase *p, const SceNpId *me);
int tss_contexts(const SceNpCommunicationId *c, const SceNpCommunicationPassphrase *p, const SceNpId *me)
{
	SceNpTssIfType iftype = SCE_NP_TSS_IFTYPE_IF_RANGE;
	int title = sceNpTssCreateTitleCtx(c, p, me);
	int req = sceNpTssCreateTransactionCtx(title);
	sceNpTssDestroyTransactionCtx(req);
	return sceNpTssDestroyTitleCtx(title) + iftype;
}

int main(void)
{
	return 0;
}

/* <np/error.h>: every NP error code, beside the NP headers that define
 * some of them too.  np-error-codes-test.sh compiles this with the error
 * header first and last (NP_ERROR_LAST), in C and C++, ILP32 and LP64, under
 * -Werror: a code spelled differently in two headers would be a redefinition.
 * The asserts pin codes from across the libraries. */
#ifndef NP_ERROR_LAST
#include <np/error.h>
#endif
#include <cell/np/clans.h>
#include <cell/np/commerce2.h>
#include <cell/np/common.h>
#include <cell/np/signaling.h>
#include <cell/np/sns.h>
#include <cell/np/trophy.h>
#include <cell/np/tus.h>
#include <cell/np2/init.h>
#include <cell/np2/matching2.h>
#ifdef NP_ERROR_LAST
#include <np/error.h>
#endif

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif
#define CODE(name, value) STATIC_ASSERT((unsigned)(name) == (value), #name)

CODE(SCE_NP_AUTH_ERROR_ACCOUNT_RENEW_ACCOUNT4, 0x8002a243u);
CODE(SCE_NP_AUTH_ERROR_INVALID_ENTITLEMENT_ID, 0x8002a222u);
CODE(SCE_NP_BASIC_ERROR_DATA_LOST, 0x8002a66eu);
CODE(SCE_NP_COMMERCE_ERROR_CHECKOUT_MAINTENANCE, 0x80029434u);
CODE(SCE_NP_COMMERCE_ERROR_UNKNOWN, 0x80029411u);
CODE(SCE_NP_COMMUNITY_SERVER_ERROR_BLACKLISTED_USER_ID, 0x8002a411u);
CODE(SCE_NP_COMMUNITY_SERVER_ERROR_UBS_MAINTENANCE, 0x8002a4b2u);
CODE(SCE_NP_CORE_ERROR_UNDEFINED_STREAM_ERROR, 0x8002a54du);
CODE(SCE_NP_CORE_SERVER_ERROR_CONFLICT, 0x8002a303u);
CODE(SCE_NP_DRM_SERVER_ERROR_SERVICE_STOPS_TEMPORARILY, 0x80029701u);
CODE(SCE_NP_MATCHING2_ERROR_CREATE_HEAP, 0x8002233du);
CODE(SCE_NP_MATCHING_ERROR_INVALID_REQ_ID, 0x8002a717u);
CODE(SCE_NP_MATCHING_SERVER_ERROR_MAINTENANCE, 0x8002a741u);
CODE(SCE_NP_TROPHY_ERROR_LOCKED, 0x80022912u);
/* codes the other NP headers already defined */
CODE(SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED, 0x80022701u);
CODE(SCE_NP_CORE_ERROR_INVALID_PSHANDLE, SCE_NP_CORE_ERROR_INVALID_ONLINEID);

int main(void)
{
	return 0;
}

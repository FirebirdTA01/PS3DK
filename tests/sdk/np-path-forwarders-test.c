/* tests/sdk/np-path-forwarders-test.c
 *
 * Reference TU for the np/ tree forwarders. Each forwarder resolves
 * <np/X.h> to its cell/ target:
 *   np/basic.h        -> cell/np/basic.h
 *   np/custom_menu.h  -> cell/np/custom_menu.h
 *   np/friendlist.h   -> cell/np/friendlist.h
 *   np/lookup.h       -> cell/np/lookup.h
 *   np/manager.h      -> cell/np/manager.h
 *   np/profile.h      -> cell/np/profile.h
 *   np/ranking.h      -> cell/np/score.h
 *   np/signaling.h    -> cell/np/signaling.h
 *   np/sns.h          -> cell/np/sns.h
 *   np/util.h         -> cell/np/util.h + cell/np/init.h
 *   np/auth_oauth.h   -> cell/np2/init.h
 *
 * The compiler must accept this file (under -Wall -Wextra -Werror, C and
 * C++) as long as every <np/X.h> header resolves to a target that
 * declares the pinned function (or, for util/auth_oauth, its whole
 * surface).  Pinned, one representative function per forwarder:
 *   basic        sceNpBasicInit(void)
 *   custom_menu  sceNpCustomMenuRegisterActions(const SceNpCustomMenu *,
 *                   SceNpCustomMenuEventHandler, void *, uint64_t)
 *   friendlist   sceNpFriendlist(SceNpFriendlistResultHandler, void *,
 *                   sys_memory_container_t)
 *   lookup       sceNpLookupInit(void)
 *   manager      sceNpManagerInit(void)
 *   profile      sceNpProfileCallGui(const SceNpId *,
 *                   SceNpProfileResultHandler, void *, uint64_t)
 *   ranking      sceNpScoreInit(void)            (from cell/np/score.h)
 *   signaling    sceNpSignalingInit(void)
 *   sns          sceNpSnsFbInit(const SceNpSnsFbInitParams *)
 *   util         sceNpUtilBandwidthTestInitStart(int, size_t)
 *   auth_oauth   sceNp2Init(uint32_t, void *)       (from cell/np2/init.h)
 *
 * Every representative is taken by address into a volatile pointer and the
 * pointers are referenced from main, so an unresolved symbol becomes a
 * link/reference error rather than being silently dropped.
 */
#include <np/basic.h>
#include <np/custom_menu.h>
#include <np/friendlist.h>
#include <np/lookup.h>
#include <np/manager.h>
#include <np/profile.h>
#include <np/ranking.h>
#include <np/signaling.h>
#include <np/sns.h>
#include <np/util.h>
#include <np/auth_oauth.h>

typedef int (np_fn_basic)(void);
typedef int (np_fn_custom_menu)(const SceNpCustomMenu *, SceNpCustomMenuEventHandler, void *, uint64_t);
typedef int (np_fn_friendlist)(SceNpFriendlistResultHandler, void *, sys_memory_container_t);
typedef int (np_fn_lookup)(void);
typedef int (np_fn_manager)(void);
typedef int (np_fn_profile)(const SceNpId *, SceNpProfileResultHandler, void *, uint64_t);
typedef int (np_fn_ranking)(void);
typedef int (np_fn_signaling)(void);
typedef int (np_fn_sns)(const SceNpSnsFbInitParams *);
typedef int (np_fn_util)(int, size_t);
typedef int (np_fn_auth_oauth)(uint32_t, void *);

static np_fn_basic *volatile np_p_basic = &sceNpBasicInit;
static np_fn_custom_menu *volatile np_p_custom_menu = &sceNpCustomMenuRegisterActions;
static np_fn_friendlist *volatile np_p_friendlist = &sceNpFriendlist;
static np_fn_lookup *volatile np_p_lookup = &sceNpLookupInit;
static np_fn_manager *volatile np_p_manager = &sceNpManagerInit;
static np_fn_profile *volatile np_p_profile = &sceNpProfileCallGui;
static np_fn_ranking *volatile np_p_ranking = &sceNpScoreInit;
static np_fn_signaling *volatile np_p_signaling = &sceNpSignalingInit;
static np_fn_sns *volatile np_p_sns = &sceNpSnsFbInit;
static np_fn_util *volatile np_p_util = &sceNpUtilBandwidthTestInitStart;
static np_fn_auth_oauth *volatile np_p_auth_oauth = &sceNp2Init;

int main(void)
{
    int sum = 0;
    sum += (np_p_basic != 0);
    sum += (np_p_custom_menu != 0);
    sum += (np_p_friendlist != 0);
    sum += (np_p_lookup != 0);
    sum += (np_p_manager != 0);
    sum += (np_p_profile != 0);
    sum += (np_p_ranking != 0);
    sum += (np_p_signaling != 0);
    sum += (np_p_sns != 0);
    sum += (np_p_util != 0);
    sum += (np_p_auth_oauth != 0);
    return (sum == 11) ? 0 : 1;
}

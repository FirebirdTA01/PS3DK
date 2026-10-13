# np/ header paths — PlayStation Network (NP) include surface

| Property | Value |
|---|---|
| Forwarding headers | `<np/common.h>`, `<np/ticket.h>`, `<np/tustorage.h>`, `<np/error.h>`, `<np/drm.h>` |
| Underlying surface | `<cell/np/*.h>`, `<cell/np2/*.h>` |
| Availability | header-only forwards to the existing `cell/np` surface; no SPRX needed for the include paths themselves. |

The `np/` tree is a second spelling of the same NP surface already under
`cell/np`. Each header below is a forwarder whose only content is the
`#include` line(s) pulling in the `cell/np` declaration(s) it stands for;
including one before or after any other member of the family is fine — the
guards in the underlying `cell/np` headers do the deduplication.

## <np/common.h>

Forwarder over the shared NP types and constants. In this tree it pulls in
`<cell/np/common.h>` (the shared NP types, sizes and language codes, and the
`SCE_NP_SUBJECT_*` accessors), `<cell/np/init.h>` (`sceNpInit` /
`sceNpTerm`, the minimum pool size and the `sceNpUtil*` npId comparisons) and
`<cell/np2/init.h>` (`sceNp2Init` / `sceNp2Term` and the auth/OAuth types).

## <np/ticket.h>

Forwarder for the ticket parameter surface. In this tree the underlying
surface is `<cell/np/common.h>` (ticket version, `SceNpTicketParamId`,
`SceNpEntitlement*`) plus `<cell/np/manager.h>` (the `sceNpManager*`
ticket calls).

## <np/tustorage.h>

Forwarder for the Title Small Storage (TSS) surface. In this tree
`<cell/np/tus.h>` carries the TUS and TSS call surface, the TUS context
and transaction types, the TSS slot / data-status / data types, and the
TSS-specific aliases.

## <np/error.h>

Forwarder over the NP error-code table (`<cell/np/error.h>` in this tree):
931 `SCE_NP_*` error codes, each individually `#ifndef`-guarded and
grouped by the NP library it belongs to. The include order relative to the
other `np/` headers in this family is immaterial.

## <np/drm.h>

Forwarder for the NP DRM surface (`<cell/np/drm.h>` in this tree): the
`SCE_NP_DRM_*` constants and the `sceNpDrm*` calls for licensing checks
(`sceNpDrmIsAvailable{,2}`, `sceNpDrmVerifyUpgradeLicense{,2}`),
purchase-time execution (`sceNpDrmExecuteGamePurchase`), time-limit queries
(`sceNpDrmGetTimelimit`), and exit-spawn (`sceNpDrmProcessExitSpawn{,2}`)
with the matching `SceNpDrmKey` / `SceNpDrmOpenArg` types.

## Subject accessor macros

`<cell/np/common.h>` declares the `SCE_NP_SUBJECT_*` macros a title uses
to read the fields of a `SceNpTicketParam` returned by
`<cell/np/manager.h>`'s `sceNpManagerGetTicketParam`. They cover the
online-id copy-out, the status word (age, suspension, chat restriction,
content rating) and the region (two-letter country code and language):

| Macro | Purpose |
|---|---|
| `SCE_NP_SUBJECT_ONLINE_ID_GET_ONLINE_ID(raw, onlineid)` | copy the raw online-ID bytes into an `SceNpOnlineId` and null-terminate |
| `SCE_NP_SUBJECT_STATUS_GET_AGE(u32)` | the age field of the status word |
| `SCE_NP_SUBJECT_STATUS_IS_SUSPENDED(u32)` | whether the account is currently suspended |
| `SCE_NP_SUBJECT_STATUS_IS_CHAT_DISABLED(u32)` | whether in-game chat is restricted |
| `SCE_NP_SUBJECT_STATUS_CONTENT_RATING(u32)` | content-rating flag |
| `SCE_NP_SUBJECT_REGION_GET_COUNTRY_CODE(raw, cc)` | copy the two-letter country code and null-terminate |
| `SCE_NP_SUBJECT_REGION_GET_LANGUAGE_CODE(raw)` | the language code byte of the region |

## sceNpTss context aliases

`<cell/np/tus.h>` in this tree declares four `#define` aliases that share
the TUS title and transaction contexts between the TUS and TSS surfaces,
so TSS titles can be created with the TUS context APIs:

| Alias | Expands to |
|---|---|
| `sceNpTssCreateTitleCtx(commId, passphrase, selfNpId)` | `sceNpTusCreateTitleCtx(commId, passphrase, selfNpId)` |
| `sceNpTssCreateTransactionCtx(titleCtxId)` | `sceNpTusCreateTransactionCtx(titleCtxId)` |
| `sceNpTssDestroyTitleCtx(titleCtxId)` | `sceNpTusDestroyTitleCtx(titleCtxId)` |
| `sceNpTssDestroyTransactionCtx(reqId)` | `sceNpTusDestroyTransactionCtx(reqId)` |

The TSS data getters (declared in `<cell/np/tus.h>` directly, not via the
alias set) are `sceNpTssGetData`, `sceNpTssGetDataAsync`,
`sceNpTssGetDataNoLimit` and `sceNpTssGetDataNoLimitAsync`.

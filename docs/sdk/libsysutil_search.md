# libsysutil_search (cellSearch) — content-search utility

| Property | Value |
|---|---|
| Header | `<cell/sysutil_search.h>`, `<cell/sysutil_search_types.h>` |
| Stub archive | `libsysutil_search_stub.a` (nidgen) |
| SPRX module | `cellSearchUtility` (part of `CELL_SYSMODULE_SYSUTIL`, 0x0015) |
| Entry points | 21 (20 in pre-475 firmware) |

## Surface

cellSearch provides save-data / video / music / photo content search across the dev_hdd0 filesystem: list searches, content searches, scene searches, and per-content metadata retrieval.

### Function table (21)

| Function | Signature |
|---|---|
| `cellSearchInitialize` | `int(CellSearchMode, sys_memory_container_t, CellSearchSystemCallback, void*)` |
| `cellSearchFinalize` | `int(void)` |
| `cellSearchStartListSearch` | `int(CellSearchListSearchType, CellSearchSortOrder, CellSearchId*)` |
| `cellSearchStartContentSearchInList` | `int(const CellSearchContentId*, CellSearchSortKey, CellSearchSortOrder, CellSearchId*)` |
| `cellSearchStartContentSearch` | `int(CellSearchContentSearchType, CellSearchSortKey, CellSearchSortOrder, CellSearchId*)` |
| `cellSearchGetContentInfoByOffset` | `int(const CellSearchId, int32_t, void*, CellSearchContentType*, CellSearchContentId*)` |
| `cellSearchGetContentInfoByContentId` | `int(const CellSearchContentId*, void*, CellSearchContentType*)` |
| `cellSearchGetOffsetByContentId` | `int(const CellSearchId, const CellSearchContentId*, int32_t*)` |
| `cellSearchGetContentInfoPath` | `int(const CellSearchContentId*, CellSearchContentInfoPath*)` |
| `cellSearchGetContentInfoPathMovieThumb` | `int(const CellSearchContentId*, CellSearchContentInfoPathMovieThumb*)` |
| `cellSearchGetContentInfoGameComment` | `int(const CellSearchContentId*, char[CELL_SEARCH_GAMECOMMENT_SIZE_MAX])` |
| `cellSearchGetContentIdByOffset` | `int(CellSearchId, int32_t, CellSearchContentType*, CellSearchContentId*, CellSearchTimeInfo*)` |
| `cellSearchPrepareFile` | `int(const char*)` |
| `cellSearchCancel` | `int(const CellSearchId)` |
| `cellSearchEnd` | `int(const CellSearchId)` |
| `cellSearchGetMusicSelectionContext` | `int(const CellSearchId, const CellSearchContentId*, CellSearchRepeatMode, CellSearchContextOption, CellMusicSelectionContext*)` |
| `cellSearchGetMusicSelectionContextOfSingleTrack` | `int(const CellSearchContentId*, CellMusicSelectionContext*)` |
| `cellSearchStartSceneSearchInVideo` | `int(const CellSearchContentId*, CellSearchSceneSearchType, CellSearchSortOrder, CellSearchId*)` |
| `cellSearchStartSceneSearch` | `int(CellSearchSceneSearchType, const char*, const char**, uint32_t, CellSearchSortKey, CellSearchSortOrder, CellSearchId*)` |
| `cellSearchGetContentInfoDeveloperData` | `int(const CellSearchContentId*, char[CELL_SEARCH_DEVELOPERDATA_LEN_MAX])` |
| `cellSearchGetContentInfoSharable` | `int(const CellSearchContentId*, CellSearchSharableType*)` |

## Error codes

All errors encode in `0x8002c8xx` (base `CELL_SYSUTIL_ERROR_BASE_SEARCH`):

| Error | Value |
|---|---|
| `CELL_SEARCH_ERROR_PARAM` | 0x8002c801 |
| `CELL_SEARCH_ERROR_BUSY` | 0x8002c802 |
| `CELL_SEARCH_ERROR_NO_MEMORY` | 0x8002c803 |
| `CELL_SEARCH_ERROR_UNKNOWN_MODE` | 0x8002c804 |
| `CELL_SEARCH_ERROR_ALREADY_INITIALIZED` | 0x8002c805 |
| `CELL_SEARCH_ERROR_NOT_INITIALIZED` | 0x8002c806 |
| `CELL_SEARCH_ERROR_FINALIZING` | 0x8002c807 |
| `CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH` | 0x8002c808 |
| `CELL_SEARCH_ERROR_CONTENT_OBSOLETE` | 0x8002c809 |
| `CELL_SEARCH_ERROR_CONTENT_NOT_FOUND` | 0x8002c80a |
| `CELL_SEARCH_ERROR_NOT_LIST` | 0x8002c80b |
| `CELL_SEARCH_ERROR_OUT_OF_RANGE` | 0x8002c80c |
| `CELL_SEARCH_ERROR_INVALID_SEARCHID` | 0x8002c80d |
| `CELL_SEARCH_ERROR_ALREADY_GOT_RESULT` | 0x8002c80e |
| `CELL_SEARCH_ERROR_NOT_SUPPORTED_CONTEXT` | 0x8002c80f |
| `CELL_SEARCH_ERROR_INVALID_CONTENTTYPE` | 0x8002c810 |
| `CELL_SEARCH_ERROR_DRM` | 0x8002c811 |
| `CELL_SEARCH_ERROR_TAG` | 0x8002c812 |
| `CELL_SEARCH_ERROR_GENERIC` | 0x8002c8ff |

Return codes: `CELL_SEARCH_OK` (0), `CELL_SEARCH_CANCELED` (1).

## Initialisation lifecycle

cellSearchInitialize is **asynchronous**. The state machine:

```
not_initialized → initializing → idle → finalizing → not_initialized
```

1. `cellSearchInitialize(CELL_SEARCH_MODE_NORMAL, container, callback, userData)` returns immediately. The actual ready state is signalled via `cellSysutilCheckCallback()` which fires the deferred `callback(CELL_SEARCH_EVENT_INITIALIZE_RESULT, CELL_OK, ...)`.
2. Pump `cellSysutilCheckCallback()` in a loop until the callback fires (or use a timer-driven pump).
3. Once idle, `cellSearchStartListSearch` / `cellSearchStartContentSearch*` / etc. are valid.
4. `cellSearchFinalize()` shuts down the utility.

The deferred-callback pump pattern (`cellSysutilCheckCallback`) applies to many cellSysutil services.

## Notable ABI quirks

### 1. SDK-version delta

`cellSearchGetContentInfoSharable` is a 475+ addition. Pre-475 firmware ships only 20 entry points. Guard with a runtime-version check when targeting older firmware.

### 2. Asynchronous initialisation

`cellSearchInitialize` returns `CELL_OK` immediately but the internal state transitions to `idle` only after `cellSysutilCheckCallback()` dispatches the deferred init callback. Until then:

- Other calls (StartListSearch, etc.) return `CELL_SEARCH_ERROR_NOT_INITIALIZED`.
- `cellSearchFinalize` during the pending state returns `CELL_SEARCH_ERROR_BUSY`.

The workaround in smoke samples:

```c
cellSearchInitialize(CELL_SEARCH_MODE_NORMAL, 0, search_cb, NULL);
for (int i = 0; i < 10; i++) {
    sys_timer_usleep(50 * 1000);
    cellSysutilCheckCallback();   /* fires the deferred init callback */
}
cellSearchFinalize();
```

This is part of the broader cellSysutil deferred-callback pattern — applies to many sysutil services (osk, msgdialog, savedata, etc.).

### 3. mode parameter constraint

`cellSearchInitialize` requires `mode == CELL_SEARCH_MODE_NORMAL` (0). Other values return `CELL_SEARCH_ERROR_UNKNOWN_MODE` (0x8002c804).

### 4. Callback non-NULL requirement

`cellSearchInitialize` rejects `func == NULL` with `CELL_SEARCH_ERROR_PARAM` (0x8002c801). A no-op callback is sufficient:

```c
static void search_cb(CellSearchEvent event, int result,
                      const void *param, void *userData) {
    (void)event; (void)result; (void)param; (void)userData;
}
```

### 5. Width-sensitive integers (time_t)

`time_t` fields in `CellSearchMusicInfo`, `CellSearchPhotoInfo`, `CellSearchVideoInfo`, and `CellSearchTimeInfo` are declared as `uint32_t` — the SPRX uses a 32-bit-pointer ABI where `time_t` is 4 bytes, while our LP64 PPU toolchain would emit 8-byte `time_t`. This is the same class of issue as the libdmux `size_t` → `uint32_t` lesson; see [libdmux.md §1](libdmux.md#1-width-sensitive-integers-in-caller-allocated-structs).

### 6. Opaque content ID

`CellSearchContentId` is a 16-byte opaque struct (`char data[16]`). Forward-declared in `<cell/sysutil_music.h>` and defined in `<cell/sysutil_search_types.h>`. Callers must not interpret the bytes directly — treat as an opaque token.

## Sample

`samples/sysutil/hello-ppu-search/` — PPU smoke test validated in RPCS3: loads SYSUTIL sysmodule, calls cellSearchInitialize with a no-op callback, pumps sysutil check-callback 10×50ms to let the async init complete, finalizes, exits cleanly.

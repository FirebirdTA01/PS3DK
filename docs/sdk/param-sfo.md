# PARAM.SFO Reference

This file is generated from `tools/sfo/registry/param-sfo.yml`. Update the registry first, then regenerate this document.

## Container Format

- Header: 20 bytes, little-endian, magic bytes `00 50 53 46`, version `0x00000101`.
- Index entry: `u16 key_off`, `u16 param_fmt`, `u32 param_len`, `u32 param_max`, `u32 data_off`.
- Formats: `0x0004` array, `0x0204` UTF-8 string, `0x0404` int32.
- Tables: key table starts after the index entries, keys are NUL-terminated, and the data table is 4-byte aligned.

## Sources

| Id | Source | Reference |
|---|---|---|
| `rpcs3-psf` | RPCS3 Loader/PSF.cpp and PSF.h parser and writer behavior | RPCS3 @ e426e444 rpcs3/Loader/PSF.cpp, rpcs3/Loader/PSF.h |
| `rpcs3-category` | RPCS3 rpcs3qt/category.h category names and PSF.cpp accept list | RPCS3 @ e426e444 rpcs3/rpcs3qt/category.h:23-46; rpcs3/Loader/PSF.cpp category accept list |
| `rpcs3-runtime` | RPCS3 runtime consumers surveyed in System.cpp, cellGame.cpp, cellAudioOut.cpp, unpkg.cpp, and cellSaveData.cpp | RPCS3 @ e426e444 Emu/System.cpp, cellGame.cpp, cellAudioOut.cpp, unpkg.cpp, cellSaveData.cpp |
| `rpcs3-game-list` | RPCS3 game_list_table.cpp observed game ATTRIBUTE bit usage citing PSDevWiki | RPCS3 @ e426e444 rpcs3/rpcs3qt/game_list_table.cpp:338-339 |
| `ps3dk-gamecontent` | sdk/include/cell/sysutil_gamecontent.h public cellGame parameter sizes | PS3DK sdk/include/cell/sysutil_gamecontent.h:108-143 |
| `sysutil-savedata` | sdk/include/cell/sysutil_savedata.h public savedata parameter sizes | PS3DK sdk/include/cell/sysutil_savedata.h:63-64,96-99,189-190 |
| `psl1ght-template` | PSL1GHT tools/sfo_pkg/sfo.xml defaults and current PS3DK template provenance | PSL1GHT tools/sfo_pkg/sfo.xml @ 73e34af; tools/sfo-pkg/PROVENANCE.md:13 |
| `sfo-c` | Current C sfo writer and on-PS3 TITLE_ID storage convention | PS3DK tools/sfo-pkg/sfo.c:544-554; RPCS3 @ e426e444 cellGame.h:221; observed retail SFO |
| `vita-fixture` | User-owned Vita param.sfo preservation fixture observed locally, bytes not committed | observed in a PS Vita param.sfo (19 keys, 912 B), 2026-08-30; not redistributed |
| `psdevwiki-gap` | PSDevWiki PARAM.SFO page required for ATTRIBUTE bit meanings; page not available to automated fetch due Cloudflare | https://www.psdevwiki.com/ps3/PARAM.SFO unavailable to automated fetch on 2026-08-30 |

## Category Values

| Code | Meaning | Source |
|---|---|---|
| `AM` | app music | `rpcs3-category` |
| `AP` | app photo | `rpcs3-category` |
| `AS` | app store | `rpcs3-category` |
| `AT` | app TV | `rpcs3-category` |
| `AV` | app video | `rpcs3-category` |
| `BV` | BC video | `rpcs3-category` |
| `WT` | web TV | `rpcs3-category` |
| `HM` | home | `rpcs3-category` |
| `CB` | network | `rpcs3-category` |
| `SF` | store frontend | `rpcs3-category` |
| `DG` | disc game | `rpcs3-category` |
| `HG` | HDD game | `rpcs3-category` |
| `2P` | PS2 game | `rpcs3-category` |
| `2G` | PS2 installed | `rpcs3-category` |
| `1P` | PS1 game | `rpcs3-category` |
| `PP` | PSP game | `rpcs3-category` |
| `MN` | PSP mini | `rpcs3-category` |
| `PE` | PSP remaster | `rpcs3-category` |
| `GD` | PS3 game data | `rpcs3-category` |
| `2D` | PS2 data | `rpcs3-category` |
| `SD` | PS3 save data | `rpcs3-category` |
| `MS` | PSP save data | `rpcs3-category` |

## Game PARAM.SFO Keys

| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes |
|---|---|---:|---|---|---|---|---|---|---|
| `TITLE` | utf8 | 128 | - | confirmed | `ps3dk-gamecontent` | - | - | Localized title fallback target when TITLE_NN for the configured language is empty. | - |
| `TITLE_00..TITLE_19` | utf8 | 128 | - | confirmed | `ps3dk-gamecontent` | - | - | RPCS3 picks TITLE_%02d by CellSysutilLang value and falls back to TITLE when empty. | Localized titles; TITLE_NN = cellGame PARAMID - 2. |
| `TITLE_ID` | utf8 | 16 | - | confirmed | `sfo-c` | No PARAM.SFO regex in RPCS3; 9-character rule is directory handling. Empty TITLE_ID is skipped. | - | Identity key for HDD game, disc update and game data lookup. | sfo.c pads to 16 bytes; cellGame API size is 10 bytes including NUL. |
| `VERSION` | utf8 | 6 | 01.00 | confirmed | `ps3dk-gamecontent` | NN.NN convention | observed | Disc/master revision and legacy fallback when APP_VER is absent; guest-writable by cellGameCreateGameData. | - |
| `APP_VER` | utf8 | 6 | 01.00 | confirmed | `ps3dk-gamecontent` | NN.NN convention | observed | Game or patch version; used by install lock and package update checks. | - |
| `PS3_SYSTEM_VER` | utf8 | 8 | 01.8000 | confirmed | `ps3dk-gamecontent` | DD.DDDD | - | RPCS3 compares it to installed firmware and boot aborts if higher; malformed values silently disable the check. | - |
| `PARENTAL_LEVEL` | integer | 4 | 0 | confirmed | `ps3dk-gamecontent` | range 1..11 | confirmed | Surfaced to guests; RPCS3 does not enforce parental controls. | RPCS3 UI notes only 1,2,3,5,7,9 are normally used. |
| `RESOLUTION` | integer | 4 | 63 | confirmed | `rpcs3-psf` | - | - | If configured video mode is absent from the mask, RPCS3 logs an error and forces 720p. | - |
| `SOUND_FORMAT` | integer | 4 | 279 | confirmed | `rpcs3-psf` | - | - | Builds the cellAudioOut supported-mode list; absent value defaults to LPCM 2ch. | - |
| `ATTRIBUTE` | integer | 4 | 0 | gap | `psdevwiki-gap` | - | - | Copied verbatim to guests; RPCS3 has no game behavior except observed UI metadata. | Game ATTRIBUTE bit meanings require the saved PSDevWiki PARAM.SFO page. Do not confuse this with CELL_GAME_ATTRIBUTE_* launch flags. All other bits remain gap. |
| `BOOTABLE` | integer | 4 | 1 | observed | `psl1ght-template` | - | - | RPCS3 UI metadata only; BOOTABLE=0 still boots. | - |
| `CATEGORY` | utf8 | 4 | HG | confirmed | `rpcs3-category` | - | - | Controls boot path and mounting; empty CATEGORY in a non-empty SFO aborts boot as corrupted. | - |
| `LICENSE` | utf8 | 512 | - | observed | `psl1ght-template` | - | - | Ignored by RPCS3; preserve. | - |
| `CONTENT_ID` | utf8 | 48 | - | observed | `vita-fixture` | - | - | RPCS3 reads it for trial unlock checks on CATEGORY HG; our pkg.c does not consume it. | Our package content ID comes from the package header CLI. |
| `NP_COMMUNICATION_ID` | utf8 | 16 | - | observed | `vita-fixture` | - | - | No RPCS3 behavior found; preserve. | - |
| `REGION_DENY` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - |
| `TARGET_APP_VER` | utf8 | 6 | - | confirmed | `rpcs3-runtime` | NN.NN convention | observed | For CATEGORY GD packages, exact-match install gate against the installed APP_VER. | - |
| `PATCH_FILE_NAME` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - |
| `ITEM_PRIORITY` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - |
| `INSTALL_DIR` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; package install destination comes from PKG metadata. | - |
| `XMB_APPS` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - |

### `PARENTAL_LEVEL` Values

| Value | Meaning | Confidence | Source |
|---:|---|---|---|
| `0` | unset | observed | `rpcs3-runtime` |
| `1` | 0+ | observed | `rpcs3-runtime` |
| `2` | 3+ | observed | `rpcs3-runtime` |
| `3` | 7+ | observed | `rpcs3-runtime` |
| `4` | 10+ | observed | `rpcs3-runtime` |
| `5` | 12+ | observed | `rpcs3-runtime` |
| `6` | 15+ | observed | `rpcs3-runtime` |
| `7` | 16+ | observed | `rpcs3-runtime` |
| `8` | 17+ | observed | `rpcs3-runtime` |
| `9` | 18+ | observed | `rpcs3-runtime` |
| `10` | Level 10 | observed | `rpcs3-runtime` |
| `11` | Level 11 | observed | `rpcs3-runtime` |

### `RESOLUTION` Flags

| Mask | Name | Meaning | Confidence | Source |
|---:|---|---|---|---|
| `0x1` | `ntsc` | 480 | confirmed | `rpcs3-psf` |
| `0x2` | `pal` | 576 | confirmed | `rpcs3-psf` |
| `0x4` | `hd_720` | 720 | confirmed | `rpcs3-psf` |
| `0x8` | `hd_1080` | 1080 | confirmed | `rpcs3-psf` |
| `0x10` | `ntsc_16_9` | 480 16:9 | confirmed | `rpcs3-psf` |
| `0x20` | `pal_16_9` | 576 16:9 | confirmed | `rpcs3-psf` |

### `SOUND_FORMAT` Flags

| Mask | Name | Meaning | Confidence | Source |
|---:|---|---|---|---|
| `0x1` | `lpcm_2` | LPCM 2ch | confirmed | `rpcs3-psf` |
| `0x4` | `lpcm_5_1` | LPCM 5.1 | confirmed | `rpcs3-psf` |
| `0x10` | `lpcm_7_1` | LPCM 7.1 | confirmed | `rpcs3-psf` |
| `0x100` | `dolby_digital_5_1` | Dolby Digital 5.1 | confirmed | `rpcs3-psf` |
| `0x200` | `dts_5_1` | DTS 5.1 | confirmed | `rpcs3-psf` |

### `ATTRIBUTE` Flags

| Mask | Name | Meaning | Confidence | Source |
|---:|---|---|---|---|
| `0x800000` | `ps_move_support` | PS Move support | observed | `rpcs3-game-list` |

## Savedata PARAM.SFO Keys

| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes |
|---|---|---:|---|---|---|---|---|---|---|
| `TITLE` | utf8 | 128 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. |
| `SUB_TITLE` | utf8 | 128 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. |
| `DETAIL` | utf8 | 1024 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. |
| `ATTRIBUTE` | integer | 4 | - | confirmed | `sysutil-savedata` | - | - | Savedata metadata bitfield generated and read separately from game ATTRIBUTE. | Savedata ATTRIBUTE is separate from game PARAM.SFO ATTRIBUTE. Name from RPCS3 savedata generator; bit from sysutil_savedata.h. |
| `SAVEDATA_LIST_PARAM` | utf8 | 8 | - | confirmed | `sysutil-savedata` | [A-Z0-9-_] only in RPCS3 cellSaveData. | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. |
| `SAVEDATA_DIRECTORY` | utf8 | 32 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. |
| `PARAMS` | utf8 | 1024 | - | confirmed | `rpcs3-runtime` | - | - | Written blank by RPCS3 savedata generation; not read. | - |
| `PARAMS2` | utf8 | 12 | - | confirmed | `rpcs3-runtime` | - | - | Written blank by RPCS3 savedata generation; not read. | - |
| `ACCOUNT_ID` | array | 16 | - | confirmed | `rpcs3-runtime` | - | - | RPCS3 writes sixteen ASCII zero bytes when generating savedata. | - |
| `PARENTAL_LEVEL` | integer | 4 | - | confirmed | `rpcs3-runtime` | - | - | - | - |
| `CATEGORY` | utf8 | 4 | SD | confirmed | `rpcs3-category` | - | - | - | - |
| `RPCS3_BLIST` | utf8 | - | - | observed | `rpcs3-runtime` | - | - | - | RPCS3 extension for savedata file order; preserve and do not apply PS3 validation. |
| `*<filename>` | integer | 4 | - | observed | `rpcs3-runtime` | - | - | - | RPCS3 extension pattern for savedata secure-file flags; preserve and do not apply PS3 validation. |

### `ATTRIBUTE` Flags

| Mask | Name | Meaning | Confidence | Source |
|---:|---|---|---|---|
| `0x1` | `no_duplicate` | No duplicate | confirmed | `sysutil-savedata` |

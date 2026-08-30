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
| `psdevwiki-gap` | PARAM.SFO keys whose public source data is still incomplete | Rows intentionally preserved as unknown until a public source confirms their format or behavior |
| `psdevwiki` | PSDevWiki PARAM.SFO page saved by the director | PSDevWiki PARAM.SFO, page saved 2026-08-30; local text lines cited in tools/sfo/registry/param-sfo.yml |

## Category Values

| Code | Meaning | Confidence | Source |
|---|---|---|---|
| `AM` | app music | confirmed | `rpcs3-category` |
| `AP` | Application Photo | confirmed | `psdevwiki` |
| `AS` | Application Streaming? | speculative | `psdevwiki` |
| `AT` | Application TV | confirmed | `psdevwiki` |
| `AV` | app video | confirmed | `rpcs3-category` |
| `BV` | BC video | confirmed | `rpcs3-category` |
| `WT` | Web TV | confirmed | `psdevwiki` |
| `HM` | home | confirmed | `rpcs3-category` |
| `CB` | network | confirmed | `rpcs3-category` |
| `SF` | store frontend | confirmed | `rpcs3-category` |
| `DG` | disc game | confirmed | `rpcs3-category` |
| `HG` | HDD game | confirmed | `rpcs3-category` |
| `2P` | PS2 game | confirmed | `rpcs3-category` |
| `2G` | PS2 installed | confirmed | `rpcs3-category` |
| `1P` | PS1 game | confirmed | `rpcs3-category` |
| `PP` | PSP game | confirmed | `rpcs3-category` |
| `MN` | PSP mini | confirmed | `rpcs3-category` |
| `PE` | PSP remaster | confirmed | `rpcs3-category` |
| `GD` | PS3 game data | confirmed | `rpcs3-category` |
| `2D` | PS2 data | confirmed | `rpcs3-category` |
| `SD` | PS3 save data | confirmed | `rpcs3-category` |
| `MS` | PSP save data | confirmed | `rpcs3-category` |
| `TR` | disc subfolder: PS3_CONTENT/THEMEDIR/ | observed | `psdevwiki` |
| `VR` | disc subfolder: VIDEODIR/ | observed | `psdevwiki` |
| `DP` | disc subfolder: PKGDIR/ | observed | `psdevwiki` |
| `XR` | disc subfolder: PS3_EXTRA/ | observed | `psdevwiki` |

## Game PARAM.SFO Keys

| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes | Variance |
|---|---|---:|---|---|---|---|---|---|---|---|
| `TITLE` | utf8 | 128 | - | confirmed | `ps3dk-gamecontent` | - | - | Localized title fallback target when TITLE_NN for the configured language is empty. | - | - |
| `TITLE_00..TITLE_19` | utf8 | 128 | - | confirmed | `ps3dk-gamecontent` | - | - | RPCS3 picks TITLE_%02d by CellSysutilLang value and falls back to TITLE when empty. | Localized titles; TITLE_NN = cellGame PARAMID - 2. | - |
| `TITLE_ID` | utf8 | 16 | - | confirmed | `sfo-c` | ABCD12345 PSDevWiki convention. No PARAM.SFO regex in RPCS3; the 9-character rule is directory handling. Empty TITLE_ID is skipped. | observed | Identity key for HDD game, disc update and game data lookup. | sfo.c pads to 16 bytes; cellGame API size is 10 bytes including NUL. | - |
| `VERSION` | utf8 | 6 | 01.00 | confirmed | `ps3dk-gamecontent` | NN.NN convention | observed | Disc/master revision and legacy fallback when APP_VER is absent; guest-writable by cellGameCreateGameData. | - | - |
| `APP_VER` | utf8 | 6 | 01.00 | confirmed | `ps3dk-gamecontent` | NN.NN convention | observed | Game or patch version; used by install lock and package update checks. | - | - |
| `PS3_SYSTEM_VER` | utf8 | 8 | 01.8000 | confirmed | `ps3dk-gamecontent` | DD.DDDD | - | RPCS3 compares it to installed firmware and boot aborts if higher; malformed values silently disable the check. | - | - |
| `PARENTAL_LEVEL` | integer | 4 | 0 | confirmed | `ps3dk-gamecontent` | range 1..11 | confirmed | Surfaced to guests; RPCS3 does not enforce parental controls. | RPCS3 UI notes only 1,2,3,5,7,9 are normally used. | - |
| `RESOLUTION` | integer | 4 | 63 | confirmed | `rpcs3-psf` | - | - | If configured video mode is absent from the mask, RPCS3 logs an error and forces 720p. | - | - |
| `SOUND_FORMAT` | integer | 4 | 279 | confirmed | `rpcs3-psf` | - | - | Builds the cellAudioOut supported-mode list; absent value defaults to LPCM 2ch. | Examples: 21, 258, 279, 514, 791. Dolby Digital and DTS examples include flag 0x2. | - |
| `ATTRIBUTE` | integer | 4 | 0 | confirmed | `psdevwiki` | - | - | Copied verbatim to guests; RPCS3 has no game behavior except observed UI metadata. | Bootable-content interpretation from PSDevWiki text lines 1003-1192 and 1307-1392. Do not confuse this with CELL_GAME_ATTRIBUTE_* launch flags. Context-specific patch and disc-subfolder tables are listed separately. | - |
| `BOOTABLE` | integer | 4 | 1 | observed | `psl1ght-template` | - | - | RPCS3 UI metadata only; BOOTABLE=0 still boots. | - | - |
| `CATEGORY` | utf8 | 4 | HG | confirmed | `rpcs3-category` | - | - | Controls boot path and mounting; empty CATEGORY in a non-empty SFO aborts boot as corrupted. | - | - |
| `LICENSE` | utf8 | 512 | - | observed | `psl1ght-template` | - | - | Ignored by RPCS3; preserve. | - | - |
| `CONTENT_ID` | utf8 | 48 | - | confirmed | `psdevwiki` | 37 characters by PSDevWiki convention; our package content ID comes from the package header CLI. | observed | RPCS3 reads it for trial unlock checks on CATEGORY HG; our pkg.c does not consume it. | Observed at max 48 in the Vita fixture. | - |
| `NP_COMMUNICATION_ID` | utf8 | 16 | - | confirmed | `psdevwiki` | 13 characters by PSDevWiki convention. | observed | No RPCS3 behavior found; preserve. | Observed at max 16 in the Vita fixture. | - |
| `REGION_DENY` | integer | 4 | - | confirmed | `psdevwiki` | - | - | No RPCS3 behavior found; PSDevWiki says bit n set denies region n for HDD games, firmware 3.30. | - | - |
| `TARGET_APP_VER` | utf8 | 6 | - | confirmed | `rpcs3-runtime` | NN.NN convention | observed | For CATEGORY GD packages, exact-match install gate against the installed APP_VER. | - | - |
| `GAMEDATA_ID` | utf8 | 32 | - | speculative | `psdevwiki` | - | - | PSDevWiki lists it for game data; no RPCS3 behavior found, preserve. | PSDevWiki marks this row with uncertainty. | - |
| `PATCH_FILE_NAME` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - | - |
| `ITEM_PRIORITY` | integer | 4 | - | confirmed | `psdevwiki` | - | - | PSDevWiki lists it for CATEGORY 2G; no RPCS3 behavior found, preserve. | - | - |
| `INSTALL_DIR` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; package install destination comes from PKG metadata. | - | - |
| `XMB_APPS` | unknown | - | - | gap | `psdevwiki-gap` | - | - | No RPCS3 behavior found; preserve. | - | - |
| `PARENTAL_LEVEL_A` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override for SCEA 01 / Americas. | - | - |
| `PARENTAL_LEVEL_C` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override for SCH 05 / China. | - | - |
| `PARENTAL_LEVEL_E` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override for SCEE 02 / Europe, Middle East, Africa, UK, Oceania and related regions. | - | - |
| `PARENTAL_LEVEL_H` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override from PSDevWiki. | - | - |
| `PARENTAL_LEVEL_J` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override from PSDevWiki. | - | - |
| `PARENTAL_LEVEL_K` | integer | 4 | - | confirmed | `psdevwiki` | - | - | License-area parental level override from PSDevWiki. | - | - |

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

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `ntsc` | - | 480 | confirmed | `rpcs3-psf` |
| `0x2` | `pal` | - | 576 | confirmed | `rpcs3-psf` |
| `0x4` | `hd_720` | - | 720 | confirmed | `rpcs3-psf` |
| `0x8` | `hd_1080` | - | 1080 | confirmed | `rpcs3-psf` |
| `0x10` | `ntsc_16_9` | - | 480 16:9 | confirmed | `rpcs3-psf` |
| `0x20` | `pal_16_9` | - | 576 16:9 | confirmed | `rpcs3-psf` |

### `SOUND_FORMAT` Flags

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `lpcm_2` | - | LPCM 2ch | confirmed | `rpcs3-psf` |
| `0x2` | `dolby_dts_required_0x2` | - | flag 0x2 (required by Dolby/DTS) | speculative | `psdevwiki` |
| `0x4` | `lpcm_5_1` | - | LPCM 5.1 | confirmed | `rpcs3-psf` |
| `0x10` | `lpcm_7_1` | - | LPCM 7.1 | confirmed | `rpcs3-psf` |
| `0x100` | `dolby_digital_5_1` | - | Dolby Digital 5.1 | confirmed | `rpcs3-psf` |
| `0x200` | `dts_5_1` | - | DTS 5.1 | confirmed | `rpcs3-psf` |

### `ATTRIBUTE` Flags

#### Portables and XMB

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `psp_remote_play_v1` | - | PSP Remote Play v1, firmware 1.10 | confirmed | `psdevwiki` |
| `0x2` | `psp_export` | - | PSP Export / Copy to PSP | confirmed | `psdevwiki` |
| `0x4` | `psp_remote_play_v2` | - | PSP Remote Play v2, firmware 1.80; requires v1 | confirmed | `psdevwiki` |
| `0x8` | `xmb_ingame_forced_enable` | - | XMB In-Game forced enabled | confirmed | `psdevwiki` |
| `0x10` | `xmb_ingame_disabled` | - | XMB In-Game disabled | confirmed | `psdevwiki` |
| `0x20` | `xmb_ingame_bgm` | - | XMB In-Game background music | confirmed | `psdevwiki` |
| `0x40` | `system_voice_chat` | - | System voice chat | speculative | `psdevwiki` |
| `0x80` | `psvita_remote_play` | - | PS Vita Remote Play, firmware 4.00 | confirmed | `psdevwiki` |

#### Warning screens

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x100` | `move_controller_warning` | - | Move Controller warning screen | confirmed | `psdevwiki` |
| `0x200` | `navigation_controller_warning` | - | Navigation Controller warning screen; requires Move warning | confirmed | `psdevwiki` |
| `0x400` | `ps_eye_warning` | - | PlayStation Eye Camera warning screen; requires Move warning | confirmed | `psdevwiki` |
| `0x800` | `move_calibration_notification` | - | Move calibration notification | confirmed | `psdevwiki` |
| `0x1000` | `stereoscopic_3d_warning` | - | Stereoscopic 3D warning | confirmed | `psdevwiki` |
| `0x2000` | `psnow_beta_notification` | - | PlayStation Now Beta notification screen | speculative | `psdevwiki` |
| `0x4000` | `reserved_0x4000` | - | Not Used Yet | reserved | `psdevwiki` |
| `0x8000` | `reserved_0x8000` | - | Not Used Yet | reserved | `psdevwiki` |

#### Disc, purchase and license

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x10000` | `install_disc` | - | Install Disc | confirmed | `psdevwiki` |
| `0x20000` | `install_packages` | - | Install Packages | confirmed | `psdevwiki` |
| `0x40000` | `unknown_0x40000` | - | Unknown for bootable content; patch overwrite flag in patch context | speculative | `psdevwiki` |
| `0x80000` | `game_purchase_enabled` | - | Game Purchase Enabled | confirmed | `psdevwiki` |
| `0x100000` | `license_related` | - | License related behavior | speculative | `psdevwiki` |
| `0x200000` | `pcengine` | - | PCEngine | speculative | `psdevwiki` |
| `0x400000` | `license_logo_disabled` | - | License Logo Disabled | confirmed | `psdevwiki` |
| `0x800000` | `move_controller_enabled` | ps_move_support | Move Controller Enabled / PS Move support; corroborated by RPCS3 game_list_table.cpp | confirmed | `psdevwiki` |

#### Emulator

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1000000` | `emulator_type_bit0` | - | Emulator type field bit 0 | speculative | `psdevwiki` |
| `0x2000000` | `emulator_type_bit1` | - | Emulator type field bit 1 | speculative | `psdevwiki` |
| `0x4000000` | `neogeo_emulator_type` | - | NeoGeo emulator type field value X4; requires PCEngine | confirmed | `psdevwiki` |
| `0x8000000` | `reserved_0x8000000` | - | Not Used Yet | reserved | `psdevwiki` |
| `0x10000000` | `reserved_0x10000000` | - | Not Used Yet | reserved | `psdevwiki` |
| `0x20000000` | `reserved_0x20000000` | - | Not Used Yet | reserved | `psdevwiki` |
| `0x40000000` | `reserved_0x40000000` | - | Not Used Yet | reserved | `psdevwiki` |
| `0x80000000` | `reserved_0x80000000` | - | Not Used Yet | reserved | `psdevwiki` |


### `REGION_DENY` Flags

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `japan` | - | Japan denied | confirmed | `psdevwiki` |
| `0x2` | `us_canada` | - | US/Canada denied | confirmed | `psdevwiki` |
| `0x4` | `europe_middle_east_africa` | - | Europe/Middle East/Africa denied | confirmed | `psdevwiki` |
| `0x8` | `korea` | - | Korea denied | confirmed | `psdevwiki` |
| `0x10` | `uk_ireland` | - | UK/Ireland denied | confirmed | `psdevwiki` |
| `0x20` | `mexico_central_south_america` | - | Mexico/Central and South America denied | confirmed | `psdevwiki` |
| `0x40` | `australia_new_zealand` | - | Australia/New Zealand denied | confirmed | `psdevwiki` |
| `0x80` | `singapore_malaysia` | - | Singapore/Malaysia denied | confirmed | `psdevwiki` |
| `0x100` | `taiwan` | - | Taiwan denied | confirmed | `psdevwiki` |
| `0x200` | `russia_ukraine_india_central_asia` | - | Russia/Ukraine/India/Central Asia denied | confirmed | `psdevwiki` |
| `0x400` | `china` | - | China denied | confirmed | `psdevwiki` |
| `0x800` | `hong_kong` | - | Hong Kong denied | confirmed | `psdevwiki` |

### `ATTRIBUTE` Bit Fields

| Mask | Name | Meaning | Confidence | Source |
|---:|---|---|---|---|
| `0x7000000` | `emulator_type_field` | 3-bit X1..X7 emulator type field; X4 is NeoGeo | speculative | `psdevwiki` |

### `ATTRIBUTE` Disc subfolder Flags

CATEGORY TR/VR/DP/XR; PSDevWiki text lines 1193-1305 and 1393-1406.

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `subfolder_enabled` | - | Subfolder enabled | confirmed | `psdevwiki` |

### `ATTRIBUTE` Patch Flags

CATEGORY GD update packages with APP_VER/TARGET_APP_VER; PSDevWiki text lines 1407-1428.

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x40000` | `overwrite_resolution_sound_remoteplay` | - | Overwrite RESOLUTION, SOUND_FORMAT and remote-play flags from the base SFO | confirmed | `psdevwiki` |
| `0x100000` | `overwrite_xmb_ingame` | - | Overwrite XMB In-Game flags 0x8/0x10/0x20 | confirmed | `psdevwiki` |
| `0x200000` | `overwrite_move_warning` | - | Expected to overwrite Move/Nav/PS Eye warning flags | speculative | `psdevwiki` |
| `0x400000` | `overwrite_3d_warning` | - | Expected to overwrite stereoscopic 3D warning | speculative | `psdevwiki` |
| `0x800000` | `overwrite_move_enabled` | - | Expected to overwrite Move Controller Enabled | speculative | `psdevwiki` |

## Savedata PARAM.SFO Keys

| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes | Variance |
|---|---|---:|---|---|---|---|---|---|---|---|
| `TITLE` | utf8 | 128 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. | - |
| `SUB_TITLE` | utf8 | 128 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. | - |
| `DETAIL` | utf8 | 1024 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. | - |
| `ATTRIBUTE` | integer | 4 | - | confirmed | `sysutil-savedata` | - | - | Savedata metadata bitfield generated and read separately from game ATTRIBUTE. | Savedata ATTRIBUTE is separate from game PARAM.SFO ATTRIBUTE. Name from RPCS3 savedata generator; bit from sysutil_savedata.h; PSDevWiki labels the same bit Copy Protected. | - |
| `SAVEDATA_LIST_PARAM` | utf8 | 8 | - | confirmed | `sysutil-savedata` | [A-Z0-9-_] only in RPCS3 cellSaveData. | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. | - |
| `SAVEDATA_DIRECTORY` | utf8 | 32 | - | confirmed | `sysutil-savedata` | - | - | - | Name from RPCS3 savedata generator; size from sysutil_savedata.h. | - |
| `PARAMS` | array | 1024 | - | observed | `psdevwiki` | - | - | Written blank by RPCS3 savedata generation; not read. | PSDevWiki matrix says utf8-S / format 0x0004. RPCS3-generated savedata uses 0x0204 strings and marks those lines uncertain; preserve and accept either. | RPCS3-generated savedata uses 0x0204 strings while PSDevWiki says utf8-S / format 0x0004. |
| `PARAMS2` | array | 12 | - | observed | `psdevwiki` | - | - | Written blank by RPCS3 savedata generation; not read. | PSDevWiki matrix says utf8-S / format 0x0004. RPCS3-generated savedata uses 0x0204 strings and marks those lines uncertain; preserve and accept either. | RPCS3-generated savedata uses 0x0204 strings while PSDevWiki says utf8-S / format 0x0004. |
| `ACCOUNT_ID` | array | 16 | - | confirmed | `rpcs3-runtime` | - | - | RPCS3 writes sixteen ASCII zero bytes when generating savedata. | - | - |
| `PARENTAL_LEVEL` | integer | 4 | - | confirmed | `rpcs3-runtime` | - | - | - | - | - |
| `CATEGORY` | utf8 | 4 | SD | confirmed | `rpcs3-category` | - | - | - | - | - |
| `RPCS3_BLIST` | utf8 | - | - | observed | `rpcs3-runtime` | - | - | - | RPCS3 extension for savedata file order; preserve and do not apply PS3 validation. | - |
| `*<filename>` | integer | 4 | - | observed | `rpcs3-runtime` | - | - | - | RPCS3 extension pattern for savedata secure-file flags; preserve and do not apply PS3 validation. | - |

### `ATTRIBUTE` Flags

| Mask | Name | Aliases | Meaning | Confidence | Source |
|---:|---|---|---|---|---|
| `0x1` | `no_duplicate` | copy_protected | No duplicate / Copy Protected | confirmed | `sysutil-savedata` |

## Trophy PARAM.SFO Keys

| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes | Variance |
|---|---|---:|---|---|---|---|---|---|---|---|
| `TITLE` | utf8 | 128 | - | confirmed | `psdevwiki` | - | - | - | Trophy PARAM.SFO uses no CATEGORY. | - |
| `LANG` | integer | 4 | - | confirmed | `psdevwiki` | - | - | - | - | - |
| `NPCOMMID` | utf8 | 16 | - | confirmed | `psdevwiki` | 12 characters by PSDevWiki convention. | observed | - | - | - |
| `PADDING` | array | 8 | - | observed | `psdevwiki` | - | - | - | PSDevWiki lists utf8-S / format 0x0004 for this padding field. | - |
| `SOURCE` | integer | 4 | - | confirmed | `psdevwiki` | - | - | - | Usually zeroes according to PSDevWiki. | - |

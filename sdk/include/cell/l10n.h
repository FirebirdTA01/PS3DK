/*! \file cell/l10n.h
 \brief Sony-SDK-source-compat cellL10n localisation API.

  Minimal first-cut: covers the surface needed to port Sony's smallest
  l10n samples (eucjp2sjis, jis2sjis, etc.) and the UTF/SBCS/UCS2
  converter family.  The complete Sony header is ~635 lines with
  every codepage table; we ship only what samples actually need plus
  the L10nCode enum.  The full surface is on the Phase 6.5 backfill
  list — extend here as more l10n calls land in samples.

  All declarations are resolved at link time by libl10n_stub.a (see
  scripts/build-cell-stub-archives.sh).  At runtime the loader
  resolves the FNIDs against the cellL10n SPRX module — the
  application MUST first call cellSysmoduleLoadModule(CELL_SYSMODULE_L10N).
*/

#ifndef __PSL1GHT_CELL_L10N_H__
#define __PSL1GHT_CELL_L10N_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* L10nResult — return value family for the *Str / *toUTF8 / etc. functions. */
typedef enum {
	ConversionOK     = 0,
	SRCIllegal       = 1,
	DSTExhausted     = 2,
	ConverterUnknown = 3,
} L10nResult;

/* L10nCode — the codepage selector for L10nConvert et al. */
typedef enum {
	L10N_UTF8          = 0x00000001,
	L10N_UTF16         = 0x00000002,
	L10N_UTF32         = 0x00000004,
	L10N_UCS2          = 0x00000008,
	L10N_UCS4          = 0x00000010,
	L10N_ISO_8859_1    = 0x00000020,
	L10N_ISO_8859_2    = 0x00000040,
	L10N_ISO_8859_3    = 0x00000080,
	L10N_ISO_8859_4    = 0x00000100,
	L10N_ISO_8859_5    = 0x00000200,
	L10N_ISO_8859_6    = 0x00000400,
	L10N_ISO_8859_7    = 0x00000800,
	L10N_ISO_8859_8    = 0x00001000,
	L10N_ISO_8859_9    = 0x00002000,
	L10N_ISO_8859_10   = 0x00004000,
	L10N_ISO_8859_11   = 0x00008000,
	L10N_ISO_8859_13   = 0x00010000,
	L10N_ISO_8859_14   = 0x00020000,
	L10N_ISO_8859_15   = 0x00040000,
	L10N_ISO_8859_16   = 0x00080000,
	L10N_CODEPAGE_437  = 0x00100000,
	L10N_CODEPAGE_850  = 0x00200000,
	L10N_CODEPAGE_863  = 0x00400000,
	L10N_CODEPAGE_866  = 0x00800000,
	L10N_CODEPAGE_932  = 0x01000000,
	L10N_CODEPAGE_936  = 0x02000000,
	L10N_CODEPAGE_949  = 0x04000000,
	L10N_CODEPAGE_950  = 0x08000000,
	L10N_CODEPAGE_1251 = 0x10000000,
	L10N_CODEPAGE_1252 = 0x20000000,
	L10N_EUC_CN        = 0x40000000,
	L10N_EUC_JP        = 0x80000000,
	/* Sony's full enum has more codepages (737/775/852/855/857/858/860/861/
	 * 865/869/1250/1253/1254/1257 etc.); add them here as samples need them. */
} L10nCode;

typedef int32_t l10n_conv_t;

/* JIS encoding helpers (resolved by libl10n_stub.a). */
uint16_t eucjp2sjis(const uint16_t code);
uint16_t jis2sjis(const uint16_t code);
uint16_t sjis2jis(const uint16_t code);
uint16_t kuten2eucjp(const uint16_t code);

/* Generic converter API. */
l10n_conv_t l10n_get_converter(L10nCode src, L10nCode dst);
int         L10nConvert(L10nCode src_code, const void *src, L10nCode dst_code,
                        void *dst, size_t *dst_len);
L10nResult  L10nConvertStr(L10nCode src_code, const void *src, size_t *src_len,
                           L10nCode dst_code, void *dst, size_t *dst_len);

#ifdef __cplusplus
}
#endif

/* Further reference constants: version, code-page identifiers, conversion helpers. */
#define L10N_MAJOR_VERSION                           6
#define L10N_MINOR_VERSION                           0
#define L10N_PATCH_VERSION                           1
#define L10N_VERSION_MODIFIER                        ""
#define SS2                                          0x8e
#define SS3                                          0x8f
#define UTF8_MASK0                                   0xc0
#define UTF8_MASK1                                   0x80
#define UTF8_MASK2                                   0xe0
#define UTF8_MASK3                                   0xf0
#define UTF8_MASK4                                   0xf8
#define UTF8_MASK5                                   0xfc
#define UTF8_MASK6                                   0xfe
#define UTF8_OCTET0                                  0x80
#define UTF8_OCTET1                                  0x00
#define UTF8_OCTET2                                  0xc0
#define UTF8_OCTET3                                  0xe0
#define UTF8_OCTET4                                  0xf0
#define UTF8_OCTET5                                  0xf8
#define UTF8_OCTET6                                  0xfc
#define UTF16_SURROGATES_MASK1                       0xf800
#define UTF16_SURROGATES_MASK2                       0xfc00
#define UTF16_SURROGATES                             0xd800
#define UTF16_HIGH_SURROGATES                        0xd800
#define UTF16_LOW_SURROGATES                         0xdc00
#define L10N_STR_UNKNOWN                             (1 << 0)
#define L10N_STR_ASCII                               (1 << 1)
#define L10N_STR_JIS                                 (1 << 2)
#define L10N_STR_EUCJP                               (1 << 3)
#define L10N_STR_SJIS                                (1 << 4)
#define L10N_STR_UTF8                                (1 << 5)
#define L10N_STR_ILLEGAL                             (1 << 16)
#define L10N_STR_ERROR                               (1 << 17)
#define L10N_EUC_KR                                  32
#define L10N_ISO_2022_JP                             33
#define L10N_ARIB                                    34
#define L10N_HZ                                      35
#define L10N_GB18030                                 36
#define L10N_RIS_506                                 37
#define L10N_CODEPAGE_852                            38
#define L10N_CODEPAGE_1250                           39
#define L10N_CODEPAGE_737                            40
#define L10N_CODEPAGE_1253                           41
#define L10N_CODEPAGE_857                            42
#define L10N_CODEPAGE_1254                           43
#define L10N_CODEPAGE_775                            44
#define L10N_CODEPAGE_1257                           45
#define L10N_CODEPAGE_855                            46
#define L10N_CODEPAGE_858                            47
#define L10N_CODEPAGE_860                            48
#define L10N_CODEPAGE_861                            49
#define L10N_CODEPAGE_865                            50
#define L10N_CODEPAGE_869                            51
#define _L10N_CODE_                                  52
#define L10N_SHIFT_JIS                               L10N_CODEPAGE_932
#define L10N_UHC                                     L10N_CODEPAGE_949
#define L10N_GBK                                     L10N_CODEPAGE_936
#define L10N_BIG5                                    L10N_CODEPAGE_950
#define L10N_JIS                                     L10N_ISO_2022_JP
#define L10N_MUSIC_SHIFT_JIS                         L10N_RIS_506

#endif /* __PSL1GHT_CELL_L10N_H__ */

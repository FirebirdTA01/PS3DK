/* cell/codec/dmux_pamf.h - PAMF container-specific types for cellDmux.
 *
 * Clean-room header.  Declares the PAMF (PlayStation Advanced Media
 * Format) stream-specific info structs, elementary-stream-specific
 * info structs, and access-unit-specific info structs the cellDmux
 * ABI uses when demuxing PAMF streams.
 */
#ifndef __PS3DK_CELL_CODEC_DMUX_PAMF_H__
#define __PS3DK_CELL_CODEC_DMUX_PAMF_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PAMF stream specific info ---------------------------------- */

typedef struct {
    uint32_t thisSize;
    bool     programEndCodeCb;
} CellDmuxPamfSpecificInfo;

/* ---- codec profile / level enums --------------------------------- */

/* MPEG-2 Video levels */
typedef enum {
    CELL_DMUX_PAMF_M2V_MP_LL  = 0,
    CELL_DMUX_PAMF_M2V_MP_ML,
    CELL_DMUX_PAMF_M2V_MP_H14,
    CELL_DMUX_PAMF_M2V_MP_HL
} CellDmuxPamfM2vLevel;

/* AVC/H.264 levels */
typedef enum {
    CELL_DMUX_PAMF_AVC_LEVEL_2P1 = 21,
    CELL_DMUX_PAMF_AVC_LEVEL_3P0 = 30,
    CELL_DMUX_PAMF_AVC_LEVEL_3P1 = 31,
    CELL_DMUX_PAMF_AVC_LEVEL_3P2 = 32,
    CELL_DMUX_PAMF_AVC_LEVEL_4P1 = 41,
    CELL_DMUX_PAMF_AVC_LEVEL_4P2 = 42,
} CellDmuxPamfAvcLevel;

/* ---- access-unit specific info structs -------------------------- */

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfAuSpecificInfoM2v;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfAuSpecificInfoAvc;

typedef struct {
    uint8_t channelAssignmentInfo;
    uint8_t samplingFreqInfo;
    uint8_t bitsPerSample;
} CellDmuxPamfAuSpecificInfoLpcm;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfAuSpecificInfoAc3;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfAuSpecificInfoAtrac3plus;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfAuSpecificInfoUserData;

/* ---- elementary-stream specific info structs -------------------- */

typedef struct {
    uint32_t profileLevel;
} CellDmuxPamfEsSpecificInfoM2v;

typedef struct {
    uint32_t level;
} CellDmuxPamfEsSpecificInfoAvc;

typedef struct {
    uint32_t samplingFreq;
    uint32_t numOfChannels;
    uint32_t bitsPerSample;
} CellDmuxPamfEsSpecificInfoLpcm;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfEsSpecificInfoAc3;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfEsSpecificInfoAtrac3plus;

typedef struct {
    uint32_t reserved1;
} CellDmuxPamfEsSpecificInfoUserData;

/* ---- LPCM / audio constant enums -------------------------------- */

typedef enum {
    CELL_DMUX_PAMF_FS_48K = 48000
} CellDmuxPamfSamplingFrequency;

typedef enum {
    CELL_DMUX_PAMF_BITS_PER_SAMPLE_16 = 16,
    CELL_DMUX_PAMF_BITS_PER_SAMPLE_24 = 24
} CellDmuxPamfBitsPerSample;

typedef enum {
    CELL_DMUX_PAMF_LPCM_CH_M1              = 1,
    CELL_DMUX_PAMF_LPCM_CH_LR             = 3,
    CELL_DMUX_PAMF_LPCM_CH_LRCLSRSLFE     = 9,
    CELL_DMUX_PAMF_LPCM_CH_LRCLSCS1CS2RSLFE = 11
} CellDmuxPamfLpcmChannelAssignmentInfo;

typedef enum {
    CELL_DMUX_PAMF_LPCM_FS_48K = 1
} CellDmuxPamfLpcmFs;

typedef enum {
    CELL_DMUX_PAMF_LPCM_BITS_PER_SAMPLE_16 = 1,
    CELL_DMUX_PAMF_LPCM_BITS_PER_SAMPLE_24 = 3
} CellDmuxPamfLpcmBitsPerSamples;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_CODEC_DMUX_PAMF_H__ */

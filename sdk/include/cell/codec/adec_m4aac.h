/* cell/codec/adec_m4aac.h - MPEG-4 AAC decoding with cellAdec
 * (CELL_ADEC_TYPE_M4AAC): configuration (ADIF, ADTS or raw data blocks),
 * the stream information reported per frame, error codes. */
#ifndef PS3TC_CELL_CODEC_ADEC_M4AAC_H
#define PS3TC_CELL_CODEC_ADEC_M4AAC_H

#include <stdint.h>
#include <cell/error.h>



#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t configNumber;              /* CELL_ADEC_M4AAC_CONFIG_NUMBER_* */
	union {
		struct {
			uint32_t programNumber;
		} adifConfig;
		struct {
			uint32_t samplingFreqIndex;
			uint32_t profile;
		} rawDataBlockConfig;
	} configInfo;
	uint32_t enableDownmix;
} CellAdecParamM4Aac;

typedef struct {
	uint32_t samplingFreq;
	uint32_t numberOfChannels;
	uint32_t numberOfFrontChannels;
	uint32_t numberOfFrontMonoChannels;
	uint32_t numberOfSideChannels;
	uint32_t numberOfBackChannels;
	uint32_t numberOfLfeChannels;
	uint32_t enableSBR;
	uint32_t SBRUpsamplingFactor;
	uint32_t isBsiValid;
	uint32_t configNumber;
	union {
		struct {                            /* ADIF header */
			uint32_t copyrightIdPresent;
			char     copyrightId[9];
			uint32_t originalCopy;
			uint32_t home;
			uint32_t bitstreamType;
			uint32_t bitrate;
			uint32_t numberOfProgramConfigElements;
			uint32_t bufferFullness;
		} adif;
		struct {                            /* ADTS header */
			uint32_t id;
			uint32_t layer;
			uint32_t protectionAbsent;
			uint32_t profile;
			uint32_t samplingFreqIndex;
			uint32_t privateBit;
			uint32_t channelConfiguration;
			uint32_t originalCopy;
			uint32_t home;
			uint32_t copyrightIdBit;
			uint32_t copyrightIdStart;
			uint32_t frameLength;
			uint32_t bufferFullness;
			uint32_t numberOfRawDataBlocks;
			uint32_t crcCheck;
		} adts;
	} bsi;
	struct {
		uint32_t matrixMixdownPresent;
		uint32_t mixdownIndex;
		uint32_t pseudoSurroundEnable;
	} matrixMixdown;
	uint32_t reserved;
} __attribute__((aligned(16))) CellAdecM4AacInfo;

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_CODEC_ADEC_M4AAC_H */

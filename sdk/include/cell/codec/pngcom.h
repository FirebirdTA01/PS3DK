/* cell/codec/pngcom.h — shared PNG ancillary chunk types. Clean-room.
   PRXPTR on pointer fields that cross SPRX boundary. */
#ifndef __PS3DK_CELL_CODEC_PNGCOM_H__
#define __PS3DK_CELL_CODEC_PNGCOM_H__
#include <stdint.h>
#ifdef __PPU__
#include <ppu-types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CELL_PNG_TEXT=0, CELL_PNG_ZTXT=1, CELL_PNG_ITXT=2 } CellPngTxtType;
typedef struct { uint8_t r,g,b; } CellPngPLTEentry;
typedef struct { uint16_t r,g,b,a,frequency; } CellPngPaletteEntries;
typedef struct { char *paletteName ATTRIBUTE_PRXPTR; uint8_t sampleDepth; CellPngPaletteEntries *paletteEntries ATTRIBUTE_PRXPTR; uint32_t paletteEntriesNumber; } CellPngSPLTentry;
typedef enum { CELL_PNG_BEFORE_PLTE=1, CELL_PNG_BEFORE_IDAT=2, CELL_PNG_AFTER_IDAT=8 } CellPngUnknownLocation;
typedef struct { CellPngTxtType txtType; char *keyword ATTRIBUTE_PRXPTR, *text ATTRIBUTE_PRXPTR; uint32_t textLength; char *languageTag ATTRIBUTE_PRXPTR, *translatedKeyword ATTRIBUTE_PRXPTR; } CellPngTextInfo;
typedef struct { uint32_t paletteEntriesNumber; CellPngPLTEentry *paletteEntries ATTRIBUTE_PRXPTR; } CellPngPLTE;
typedef struct { double gamma; } CellPngGAMA;
typedef struct { uint32_t renderingIntent; } CellPngSRGB;
typedef struct { char *profileName ATTRIBUTE_PRXPTR, *profile ATTRIBUTE_PRXPTR; uint32_t profileLength; } CellPngICCP;
typedef struct { uint32_t r,g,b,gray,alpha; } CellPngSBIT;
typedef struct { char *alphaForPaletteIndex ATTRIBUTE_PRXPTR; uint32_t alphaForPaletteIndexNumber; uint16_t r,g,b,gray; } CellPngTRNS;
typedef struct { uint16_t *histgramEntries ATTRIBUTE_PRXPTR; uint32_t histgramEntriesNumber; } CellPngHIST;
typedef struct { uint16_t y;uint8_t mo,d,h,mi,s; } CellPngTIME;
typedef struct { uint8_t paletteIndex; uint32_t r,g,b,gray; } CellPngBKGD;
typedef struct { CellPngSPLTentry *sPLTentries ATTRIBUTE_PRXPTR; uint32_t sPLTentriesNumber; } CellPngSPLT;
typedef struct { int32_t xPosition,yPosition; uint32_t unitSpecifier; } CellPngOFFS;
typedef struct { uint32_t xAxis,yAxis,unitSpecifier; } CellPngPHYS;
typedef struct { uint32_t unitSpecifier; double pixelWidth,pixelHeight; } CellPngSCAL;
typedef struct { double whitePointX,whitePointY,redX,redY,greenX,greenY,blueX,blueY; } CellPngCHRM;
typedef struct { char *calibrationName ATTRIBUTE_PRXPTR; int32_t x0,x1; uint32_t equationType,numberOfParameters; char *unitName ATTRIBUTE_PRXPTR, **parameter ATTRIBUTE_PRXPTR; } CellPngPCAL;
typedef struct { char chunkType[5]; char *chunkData ATTRIBUTE_PRXPTR; uint32_t length; CellPngUnknownLocation location; } CellPngUnknownChunk;
#ifdef __cplusplus
}
#endif
#endif

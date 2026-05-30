/*
 * cell/face/util.h — inline param-initialization helpers for the face API.
 *
 * Each helper fills a CellFace*Param struct with image metadata,
 * dictionary EA pointers, face detection coordinates, and result
 * buffer addresses.  Call these before dispatching to SPU-side
 * cellFace*() processing functions.
 *
 * PPU-only — relies on cellFace*GetDictEa() from cell/face/face.h.
 */
#ifndef __PS3DK_CELL_FACE_UTIL_H__
#define __PS3DK_CELL_FACE_UTIL_H__

#include <cell/face/face.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Working-area size macro                                            */
/* ------------------------------------------------------------------ */

#define CELL_FACE_UTIL_WORK_SIZE(width, height, rowstride)            \
    (240                                                              \
     + (((((uint32_t)(rowstride) /   2) + 0xf) & (~0xf)) * ((uint32_t)(height) /   2)) \
     + (((((uint32_t)(rowstride) /   4) + 0xf) & (~0xf)) * ((uint32_t)(height) /   4)) \
     + (((((uint32_t)(rowstride) /   8) + 0xf) & (~0xf)) * ((uint32_t)(height) /   8)) \
     + (((((uint32_t)(rowstride) /  16) + 0xf) & (~0xf)) * ((uint32_t)(height) /  16)) \
     + (((((uint32_t)(rowstride) /  32) + 0xf) & (~0xf)) * ((uint32_t)(height) /  32)) \
     + (((((uint32_t)(rowstride) /  64) + 0xf) & (~0xf)) * ((uint32_t)(height) /  64)) \
     + (((((uint32_t)(rowstride) / 128) + 0xf) & (~0xf)) * ((uint32_t)(height) / 128)) \
     + (((((uint32_t)(rowstride) / 256) + 0xf) & (~0xf)) * ((uint32_t)(height) / 256)) \
     + (((((uint32_t)(rowstride) / 512) + 0xf) & (~0xf)) * ((uint32_t)(height) / 512)) \
     + (((((uint32_t)(rowstride) /1024) + 0xf) & (~0xf)) * ((uint32_t)(height) /1024)))

/* ------------------------------------------------------------------ */
/*  Detection-param initializers                                       */
/* ------------------------------------------------------------------ */

static inline void
cellFaceUtilDetectionParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    /* image source */
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;

    /* work + result buffers */
    detectParam->eaWorkingArea = (uintptr_t)workingArea;
    detectParam->eaFaceResult  = (uintptr_t)faceResult;

    /* dictionary for standard detection */
    detectParam->eaDetectionDict = cellFaceDetectionGetDictEa();

    /* scan defaults */
    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;

    /* optional sub-params disabled */
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

static inline void
cellFaceUtilDetection3DParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;
    detectParam->eaWorkingArea  = (uintptr_t)workingArea;
    detectParam->eaFaceResult   = (uintptr_t)faceResult;

    /* 3D-pose dictionary */
    detectParam->eaDetectionDict = cellFaceDetection3DGetDictEa();

    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

static inline void
cellFaceUtilDetectionFrontalParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;
    detectParam->eaWorkingArea  = (uintptr_t)workingArea;
    detectParam->eaFaceResult   = (uintptr_t)faceResult;

    /* frontal uses no dictionary (embedded in SPRX) */
    detectParam->eaDetectionDict = 0;

    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

static inline void
cellFaceUtilDetectionFrontalYaw3DParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;
    detectParam->eaWorkingArea  = (uintptr_t)workingArea;
    detectParam->eaFaceResult   = (uintptr_t)faceResult;

    /* frontal + yaw 3D dictionary */
    detectParam->eaDetectionDict = cellFaceDetectionFrontalYaw3DGetDictEa();

    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

static inline void
cellFaceUtilDetectionWideParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;
    detectParam->eaWorkingArea  = (uintptr_t)workingArea;
    detectParam->eaFaceResult   = (uintptr_t)faceResult;

    /* wide-angle dictionary */
    detectParam->eaDetectionDict = cellFaceDetectionWideGetDictEa();

    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

static inline void
cellFaceUtilDetectionWide2ParamInitialize(
    CellFaceDetectionParam       *detectParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *faceResult,
    uint32_t                      faceResultNum)
{
    detectParam->eaImage        = (uintptr_t)imageData;
    detectParam->imageWidth     = imageWidth;
    detectParam->imageHeight    = imageHeight;
    detectParam->imageRowstride = imageRowstride;
    detectParam->eaWorkingArea  = (uintptr_t)workingArea;
    detectParam->eaFaceResult   = (uintptr_t)faceResult;

    /* wide-2 dictionary (v2) */
    detectParam->eaDetectionDict = cellFaceDetectionWide2GetDictEa();

    detectParam->minFaceSize  = 20;
    detectParam->maxFaceSize  = (imageWidth < imageHeight)
                                    ? imageWidth : imageHeight;
    detectParam->xScanStep    = 2;
    detectParam->yScanStep    = 2;
    detectParam->scalingStep  = 0.841f;
    detectParam->detectionThreshold = 4.0f;
    detectParam->maxNumResult = faceResultNum;
    detectParam->eaOptParamMultiSpu    = 0;
    detectParam->eaOptParamLocalSearch = 0;
}

/* ------------------------------------------------------------------ */
/*  Multi-SPU / local-search / merge helpers                           */
/* ------------------------------------------------------------------ */

static inline void
cellFaceUtilOptParamMultiSpuInitialize(
    CellFaceOptParamMultiSpu *multiSpuParam,
    CellFaceDetectionParam   *detectionParam,
    uint32_t                  numTasks,
    uint32_t                  taskId)
{
    uint32_t step = detectionParam->xScanStep;

    detectionParam->eaOptParamMultiSpu = (uintptr_t)multiSpuParam;
    detectionParam->xScanStep          = step * numTasks;
    multiSpuParam->xScanStart          = step * taskId;
    multiSpuParam->yScanStart          = 0;
    multiSpuParam->storeOverlap        = 1;
}

static inline void
cellFaceUtilOptParamLocalSearchInitialize(
    CellFaceOptParamLocalSearch *localSearchParam,
    CellFaceDetectionParam      *detectionParam,
    CellFaceDetectionResultData *localData,
    uint32_t                     numLocal)
{
    detectionParam->eaOptParamLocalSearch = (uintptr_t)localSearchParam;
    localSearchParam->eaLocalData         = (uintptr_t)localData;
    localSearchParam->numLocal            = numLocal;
    localSearchParam->scanFaceSizeMin     = 0.8f;
    localSearchParam->scanFaceSizeMax     = 1.3f;
    localSearchParam->scanFaceMarginX     = 8;
    localSearchParam->scanFaceMarginY     = 4;
}

static inline void
cellFaceUtilMergeResultParamInitialize(
    CellFaceMergeResultParam      *mergeParam,
    CellFaceDetectionResultData   *mergeData,
    uint32_t                       numMerge,
    const CellFaceDetectionResult *faceResult,
    uint32_t                       faceResultNum)
{
    mergeParam->eaMergeData   = (uintptr_t)mergeData;
    mergeParam->eaFaceResult  = (uintptr_t)faceResult;
    mergeParam->numMerge      = numMerge;
    mergeParam->maxNumResult  = faceResultNum;
}

/* ------------------------------------------------------------------ */
/*  Parts / all-parts / shape-constraint initializers                   */
/* ------------------------------------------------------------------ */

static inline void
cellFaceUtilPartsParamInitialize(
    CellFacePartsParam           *partsParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *partsResult,
    const CellFacePosition        *positionResult)
{
    partsParam->eaImage         = (uintptr_t)imageData;
    partsParam->eaWorkingArea   = (uintptr_t)workingArea;
    partsParam->eaPartsDict     = cellFacePartsGetDictEa();
    partsParam->eaPartsResult   = (uintptr_t)partsResult;
    partsParam->eaPositionResult = (uintptr_t)positionResult;
    partsParam->imageWidth      = imageWidth;
    partsParam->imageHeight     = imageHeight;
    partsParam->imageRowstride  = imageRowstride;

    /* copy face rectangle + pose from detection result */
    partsParam->faceX     = face->faceX;
    partsParam->faceY     = face->faceY;
    partsParam->faceW     = face->faceW;
    partsParam->faceH     = face->faceH;
    partsParam->faceRoll  = face->faceRoll;
    partsParam->facePitch = face->facePitch;
    partsParam->faceYaw   = face->faceYaw;
}

static inline void
cellFaceUtilAllPartsParamInitialize(
    CellFaceAllPartsParam        *allPartsParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *allPartsResult)
{
    allPartsParam->eaImage        = (uintptr_t)imageData;
    allPartsParam->eaWorkingArea  = (uintptr_t)workingArea;
    allPartsParam->eaPartsDict    = cellFaceAllPartsGetDictEa();
    allPartsParam->eaShapeDict    = 0ULL;
    allPartsParam->eaPartsResult  = (uintptr_t)allPartsResult;
    allPartsParam->eaShapeResult  = 0ULL;
    allPartsParam->enablePartsBit = 0x07ffe73fffffffffULL;
    allPartsParam->imageWidth     = imageWidth;
    allPartsParam->imageHeight    = imageHeight;
    allPartsParam->imageRowstride = imageRowstride;
    allPartsParam->faceX     = face->faceX;
    allPartsParam->faceY     = face->faceY;
    allPartsParam->faceW     = face->faceW;
    allPartsParam->faceH     = face->faceH;
    allPartsParam->faceRoll  = face->faceRoll;
    allPartsParam->facePitch = face->facePitch;
    allPartsParam->faceYaw   = face->faceYaw;
}

static inline void
cellFaceUtilAllPartsWithShapeConstraintParamInitialize(
    CellFaceAllPartsParam        *allPartsParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *allPartsResult,
    const CellFacePartsResult     *allPartsShapeResult)
{
    allPartsParam->eaImage        = (uintptr_t)imageData;
    allPartsParam->eaWorkingArea  = (uintptr_t)workingArea;
    allPartsParam->eaPartsDict    = cellFaceAllPartsGetDictEa();
    allPartsParam->eaShapeDict    = cellFaceAllPartsShapeGetDictEa();
    allPartsParam->eaPartsResult  = (uintptr_t)allPartsResult;
    allPartsParam->eaShapeResult  = (uintptr_t)allPartsShapeResult;
    allPartsParam->enablePartsBit = 0x07ffe73fffffffffULL;
    allPartsParam->imageWidth     = imageWidth;
    allPartsParam->imageHeight    = imageHeight;
    allPartsParam->imageRowstride = imageRowstride;
    allPartsParam->faceX     = face->faceX;
    allPartsParam->faceY     = face->faceY;
    allPartsParam->faceW     = face->faceW;
    allPartsParam->faceH     = face->faceH;
    allPartsParam->faceRoll  = face->faceRoll;
    allPartsParam->facePitch = face->facePitch;
    allPartsParam->faceYaw   = face->faceYaw;
}

static inline void
cellFaceUtilAllPartsEnableByID(
    CellFaceAllPartsParam *allPartsParam,
    uint32_t              *listPartsId,
    uint32_t               listPartsIdNum)
{
    uint64_t mask = 0;
    uint32_t i;
    for (i = 0; i < listPartsIdNum; i++) {
        mask |= (1ULL) << (listPartsId[i] - CELL_FACE_PARTS_ID_ALL_BASE);
    }
    allPartsParam->enablePartsBit = mask;
}

static inline void
cellFaceUtilAllPartsShapeParamInitialize(
    CellFaceAllPartsShapeParam   *allPartsShapeParam,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *allPartsResult,
    const CellFacePartsResult     *allPartsShapeResult)
{
    allPartsShapeParam->eaShapeDict   = cellFaceAllPartsShapeGetDictEa();
    allPartsShapeParam->eaPartsResult = (uintptr_t)allPartsResult;
    allPartsShapeParam->eaShapeResult = (uintptr_t)allPartsShapeResult;
    allPartsShapeParam->faceYaw       = face->faceYaw;
}

/* ------------------------------------------------------------------ */
/*  Parts + attribute initializers                                     */
/* ------------------------------------------------------------------ */

static inline void
cellFaceUtilPartsAttribParamInitialize(
    CellFacePartsAttribParam     *partsAttribParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *partsResult,
    const CellFaceAttribResult    *attribResult,
    const CellFacePosition        *positionResult)
{
    partsAttribParam->eaImage          = (uintptr_t)imageData;
    partsAttribParam->imageWidth       = imageWidth;
    partsAttribParam->imageHeight      = imageHeight;
    partsAttribParam->imageRowstride   = imageRowstride;
    partsAttribParam->eaWorkingArea    = (uintptr_t)workingArea;
    partsAttribParam->eaPartsResult    = (uintptr_t)partsResult;
    partsAttribParam->eaAttribResult   = (uintptr_t)attribResult;
    partsAttribParam->eaPositionResult = (uintptr_t)positionResult;
    partsAttribParam->eaPartsDict      = cellFacePartsGetDictEa();
    partsAttribParam->eaAttribDict     = cellFaceAttribGetDictEa();
    partsAttribParam->faceX     = face->faceX;
    partsAttribParam->faceY     = face->faceY;
    partsAttribParam->faceW     = face->faceW;
    partsAttribParam->faceH     = face->faceH;
    partsAttribParam->faceRoll  = face->faceRoll;
    partsAttribParam->facePitch = face->facePitch;
    partsAttribParam->faceYaw   = face->faceYaw;
}

static inline void
cellFaceUtilPartsAttribExParamInitialize(
    CellFacePartsAttribParam     *partsAttribParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *partsResult,
    const CellFaceAttribResult    *attribResult,
    const CellFacePosition        *positionResult)
{
    partsAttribParam->eaImage          = (uintptr_t)imageData;
    partsAttribParam->imageWidth       = imageWidth;
    partsAttribParam->imageHeight      = imageHeight;
    partsAttribParam->imageRowstride   = imageRowstride;
    partsAttribParam->eaWorkingArea    = (uintptr_t)workingArea;
    partsAttribParam->eaPartsResult    = (uintptr_t)partsResult;
    partsAttribParam->eaAttribResult   = (uintptr_t)attribResult;
    partsAttribParam->eaPositionResult = (uintptr_t)positionResult;
    partsAttribParam->eaPartsDict      = cellFacePartsGetDictEa();
    partsAttribParam->eaAttribDict     = cellFaceAttribExGetDictEa();
    partsAttribParam->faceX     = face->faceX;
    partsAttribParam->faceY     = face->faceY;
    partsAttribParam->faceW     = face->faceW;
    partsAttribParam->faceH     = face->faceH;
    partsAttribParam->faceRoll  = face->faceRoll;
    partsAttribParam->facePitch = face->facePitch;
    partsAttribParam->faceYaw   = face->faceYaw;
}

/* ------------------------------------------------------------------ */
/*  Age, feature, similarity initializers                              */
/* ------------------------------------------------------------------ */

static inline void
cellFaceUtilAgeRangeParamInitialize(
    CellFaceAgeRangeParam        *ageRangeParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *parts,
    const CellFacePosition        *position,
    const CellFaceAgeRange        *ageRangeResult)
{
    ageRangeParam->eaImage          = (uintptr_t)imageData;
    ageRangeParam->imageWidth       = imageWidth;
    ageRangeParam->imageHeight      = imageHeight;
    ageRangeParam->imageRowstride   = imageRowstride;
    ageRangeParam->eaWorkingArea    = (uintptr_t)workingArea;
    ageRangeParam->eaParts          = (uintptr_t)parts;
    ageRangeParam->eaPosition       = (uintptr_t)position;
    ageRangeParam->eaAgeRangeResult = (uintptr_t)ageRangeResult;
    ageRangeParam->eaAgeRangeDict   = cellFaceAgeRangeGetDictEa();
    ageRangeParam->faceX     = face->faceX;
    ageRangeParam->faceY     = face->faceY;
    ageRangeParam->faceW     = face->faceW;
    ageRangeParam->faceH     = face->faceH;
    ageRangeParam->faceRoll  = face->faceRoll;
    ageRangeParam->facePitch = face->facePitch;
    ageRangeParam->faceYaw   = face->faceYaw;
}

static inline void
cellFaceUtilFeatureParamInitialize(
    CellFaceFeatureParam *featureParam,
    const void           *imageData,
    uint32_t              imageWidth,
    uint32_t              imageHeight,
    uint32_t              imageRowstride,
    const void           *workingArea,
    const CellFacePosition *position,
    const CellFaceFeature  *featureResult)
{
    featureParam->eaImage        = (uintptr_t)imageData;
    featureParam->imageWidth     = imageWidth;
    featureParam->imageHeight    = imageHeight;
    featureParam->imageRowstride = imageRowstride;
    featureParam->eaWorkingArea  = (uintptr_t)workingArea;
    featureParam->eaPosition     = (uintptr_t)position;
    featureParam->eaFeatureResult = (uintptr_t)featureResult;
}

static inline void
cellFaceUtilSimilarityParamInitialize(
    CellFaceSimilarityParam *similarityParam,
    const CellFaceFeature   *feature,
    const CellFaceFeature   *regFeatureArray,
    uint32_t                 numRegFeature,
    uint32_t                 strideRegFeature,
    const float             *scoreResultArray)
{
    similarityParam->eaFeature         = (uintptr_t)feature;
    similarityParam->eaRegFeatureArray = (uintptr_t)regFeatureArray;
    similarityParam->eaSimilarityDict  = cellFaceSimilarityGetDictEa();
    similarityParam->eaScoreResultArray = (uintptr_t)scoreResultArray;
    similarityParam->numRegFeature     = numRegFeature;
    similarityParam->strideRegFeature  = strideRegFeature;
}

static inline void
cellFaceUtilFeature2ParamInitialize(
    CellFaceFeature2Param *featureParam,
    const void            *imageData,
    uint32_t               imageWidth,
    uint32_t               imageHeight,
    uint32_t               imageRowstride,
    const void            *workingArea,
    const CellFacePosition *position,
    const CellFaceFeature2  *featureResult)
{
    featureParam->eaImage         = (uintptr_t)imageData;
    featureParam->imageWidth      = imageWidth;
    featureParam->imageHeight     = imageHeight;
    featureParam->imageRowstride  = imageRowstride;
    featureParam->eaWorkingArea   = (uintptr_t)workingArea;
    featureParam->eaPosition      = (uintptr_t)position;
    featureParam->eaFeatureResult = (uintptr_t)featureResult;
}

static inline void
cellFaceUtilSimilarity2ParamInitialize(
    CellFaceSimilarity2Param *similarityParam,
    const CellFaceFeature2   *feature,
    const CellFaceFeature2   *regFeatureArray,
    uint32_t                  numRegFeature,
    uint32_t                  strideRegFeature,
    const float              *scoreResultArray)
{
    similarityParam->eaFeature          = (uintptr_t)feature;
    similarityParam->eaRegFeatureArray  = (uintptr_t)regFeatureArray;
    similarityParam->eaSimilarityDict   = cellFaceSimilarity2GetDictEa();
    similarityParam->eaScoreResultArray = (uintptr_t)scoreResultArray;
    similarityParam->numRegFeature      = numRegFeature;
    similarityParam->strideRegFeature   = strideRegFeature;
}

static inline void
cellFaceUtilFeatureLightParamInitialize(
    CellFaceFeatureLightParam    *featureParam,
    const void                   *imageData,
    uint32_t                      imageWidth,
    uint32_t                      imageHeight,
    uint32_t                      imageRowstride,
    const void                   *workingArea,
    const CellFaceDetectionResult *face,
    const CellFacePartsResult     *parts,
    const CellFaceFeatureLight    *featureResult)
{
    featureParam->eaImage         = (uintptr_t)imageData;
    featureParam->imageWidth      = imageWidth;
    featureParam->imageHeight     = imageHeight;
    featureParam->imageRowstride  = imageRowstride;
    featureParam->eaWorkingArea   = (uintptr_t)workingArea;
    featureParam->eaFeatureDict   = (uintptr_t)cellFaceFeatureLightGetDictEa();
    featureParam->eaFaceResult    = (uintptr_t)face;
    featureParam->eaPartsResult   = (uintptr_t)parts;
    featureParam->eaFeatureResult = (uintptr_t)featureResult;
}

static inline void
cellFaceUtilSimilarityLightParamInitialize(
    CellFaceSimilarityLightParam *similarityParam,
    const CellFaceFeatureLight   *feature,
    const CellFaceFeatureLight   *regFeatureArray,
    uint32_t                      numRegFeature,
    uint32_t                      strideRegFeature,
    const float                  *scoreResultArray)
{
    similarityParam->eaFeature          = (uintptr_t)feature;
    similarityParam->eaRegFeatureArray  = (uintptr_t)regFeatureArray;
    similarityParam->eaScoreResultArray = (uintptr_t)scoreResultArray;
    similarityParam->numRegFeature      = numRegFeature;
    similarityParam->strideRegFeature   = strideRegFeature;
}

#endif /* __PS3DK_CELL_FACE_UTIL_H__ */

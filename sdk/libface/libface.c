#include <cell/error.h>
#include <cell/face.h>

#define CELL_FACE_DICT_EA_SENTINEL 0ULL

#define DEFINE_FACE_DICT_GETTER(name) \
    uint64_t name(void) { return CELL_FACE_DICT_EA_SENTINEL; }

DEFINE_FACE_DICT_GETTER(cellFaceDetectionGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceDetection3DGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceDetectionFrontalYaw3DGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceDetectionWideGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceDetectionWide2GetDictEa)
DEFINE_FACE_DICT_GETTER(cellFacePartsGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceAllPartsGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceAllPartsShapeGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceAttribGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceAttribExGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceAgeRangeGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceFeatureLightGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceSimilarityGetDictEa)
DEFINE_FACE_DICT_GETTER(cellFaceSimilarity2GetDictEa)

int32_t cellFaceAgeRangeIntegrate(
    CellFaceAgeRange *ageRange,
    CellFaceAgeDistr *ageDistrAccum,
    uint32_t *ageResult,
    uint32_t *ageRangeMinResult,
    uint32_t *ageRangeMaxResult)
{
    (void)ageRange;
    (void)ageDistrAccum;
    (void)ageResult;
    (void)ageRangeMinResult;
    (void)ageRangeMaxResult;
    return CELL_OK;
}

/* CELL_CAMERA_MAKE_ERROR and CELL_MICIN_MAKE_ERROR must reproduce the
 * error codes their headers already spell out as literals.  Every check is
 * a compile-time assertion, so building this file is the whole test.
 * ERROR_HELPER_NEGATIVE adds one deliberately wrong assertion, which must
 * stop the build. */
#include <cell/camera.h>
#include <cell/mic/error.h>

#ifdef __cplusplus
#define CHECK(expr, msg) static_assert(expr, msg)
#else
#define CHECK(expr, msg) _Static_assert(expr, msg)
#endif

/* The helpers yield int (CELL_ERROR_CAST); the literals are unsigned. */
#define SAME_CODE(made, literal) ((unsigned int)(made) == (unsigned int)(literal))

CHECK(CELL_ERROR_FACILITY_MICCAM == 0x014, "mic/camera facility");

CHECK(SAME_CODE(CELL_CAMERA_MAKE_ERROR(0x0801), CELL_CAMERA_ERROR_ALREADY_INIT),
      "camera ALREADY_INIT");
CHECK(SAME_CODE(CELL_CAMERA_MAKE_ERROR(0x080f), CELL_CAMERA_ERROR_FATAL),
      "camera FATAL");
CHECK(SAME_CODE(CELL_CAMERA_MAKE_ERROR(0x0810), CELL_CAMERA_ERROR_MUTEX),
      "camera MUTEX");

CHECK(SAME_CODE(CELL_MICIN_MAKE_ERROR(0x0101), CELL_MICIN_ERROR_ALREADY_INIT),
      "mic ALREADY_INIT");
CHECK(SAME_CODE(CELL_MICIN_MAKE_ERROR(0x0110), CELL_MICIN_ERROR_DEVICE_NOT_SUPPORT),
      "mic DEVICE_NOT_SUPPORT");
CHECK(SAME_CODE(CELL_MICIN_MAKE_ERROR(0x0219), CELL_MICIN_ERROR_DSP_LIB_SAMPRATE),
      "mic DSP_LIB_SAMPRATE");

/* Both helpers produce failure codes in their own facility. */
CHECK(CELL_ERROR_IS_FAILURE(CELL_CAMERA_MAKE_ERROR(0x0801)), "camera code is a failure");
CHECK(CELL_ERROR_GET_FACILITY(CELL_MICIN_MAKE_ERROR(0x0101)) == CELL_ERROR_FACILITY_MICCAM,
      "mic code carries the mic/camera facility");

#ifdef ERROR_HELPER_NEGATIVE
CHECK(SAME_CODE(CELL_CAMERA_MAKE_ERROR(0x0802), CELL_CAMERA_ERROR_ALREADY_INIT),
      "negative control: wrong status must not match");
#endif

int miccam_error_helper_probe(void)
{
    return CELL_CAMERA_MAKE_ERROR(0x0801) == (int)CELL_CAMERA_ERROR_ALREADY_INIT ? 0 : 1;
}

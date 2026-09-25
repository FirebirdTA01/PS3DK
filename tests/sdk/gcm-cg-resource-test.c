/* Independent consumer of the shipped enum API; no shader/oracle fixture.
 * An integer return breaks C++ enum assignment. Returning resIndex instead
 * of res breaks the value checks even when the declaration is correct. */
#include <cell/gcm/gcm_cg_func.h>
#include <string.h>

typedef CGresource (*ResourceQuery)(CGprogram, CGparameter);

#ifdef __cplusplus
#include <type_traits>
static_assert(std::is_enum<CGresource>::value,
              "test must use the canonical Cg enum, not the legacy typedef");
static_assert(std::is_same<decltype(&cellGcmCgGetParameterResource),
                           ResourceQuery>::value, "resource query signature");
static_assert(std::is_same<std::underlying_type<CGresource>::type,
                           uint32_t>::value, "resource return ABI");
static_assert(sizeof(CGresource) == 4, "resource return width");
#ifdef EXPECT_POINTER_SIZE
static_assert(sizeof(void *) == EXPECT_POINTER_SIZE, "selected PPU ABI");
#endif
#else
_Static_assert(sizeof(CGresource) == 4, "resource return width");
#ifdef EXPECT_POINTER_SIZE
_Static_assert(sizeof(void *) == EXPECT_POINTER_SIZE, "selected PPU ABI");
#endif
#endif

/* Keep an externally visible direct call for the relocation/symbol check. */
#ifdef __cplusplus
extern "C"
#endif
CGresource resource_consumer(CGprogram program, CGparameter parameter)
{
    CGresource resource = cellGcmCgGetParameterResource(program, parameter);
    return resource;
}

int main(void)
{
    ResourceQuery query = &cellGcmCgGetParameterResource;
    CgBinaryParameter parameter;
    memset(&parameter, 0, sizeof(parameter));
    parameter.res = CG_ATTR3;
    parameter.resIndex = 11;
    CGparameter handle = (CGparameter)&parameter;
    CGresource resource = query((CGprogram)0, handle);
    if (resource != CG_ATTR3) return 1;
    /* Existing numeric callers must retain their attribute-slot result. */
    if ((int)resource_consumer((CGprogram)0, handle) - CG_ATTR0 != 3) return 2;
    uint32_t numeric = cellGcmCgGetParameterResource((CGprogram)0, handle);
    if (numeric != (uint32_t)CG_ATTR3) return 3;
    parameter.res = CG_TEXUNIT2;
    parameter.resIndex = -1;
    if (query((CGprogram)0, handle) != CG_TEXUNIT2) return 4;
    if (resource_consumer((CGprogram)0, (CGparameter)0) != (CGresource)0) return 5;
    return 0;
}

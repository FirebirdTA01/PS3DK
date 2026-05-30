#ifndef __PS3DK_SCE_HL_SPURS_DEFINE_H__
#define __PS3DK_SCE_HL_SPURS_DEFINE_H__

#define __SCE_HL_SPURS ::sce::hl::spurs::
#define __SCE_HL_SPURS_BEGIN namespace sce { namespace hl { namespace spurs {
#define __SCE_HL_SPURS_END } } }

#define __SCE_SPURS_UTIL_RETURN_IF(exp) \
    do { int __ret = (exp); if (__ret) return __ret; } while (0)

#define __SCE_HL_SPURS_STR2(x) #x
#define __SCE_HL_SPURS_STR(x) __SCE_HL_SPURS_STR2(x)
#define __FILELINE __FILE__ ":" __SCE_HL_SPURS_STR(__LINE__) ": "

#endif /* __PS3DK_SCE_HL_SPURS_DEFINE_H__ */

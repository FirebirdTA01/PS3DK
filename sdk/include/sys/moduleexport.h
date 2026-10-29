/*
 * sys/moduleexport.h - module identity and entry-point macros for
 * CellOS Lv-2 relocatable modules (PRX), written in C or C++.
 *
 * A module source can declare its identity and entry points inline:
 *
 *     #include <sys/moduleexport.h>
 *
 *     SYS_MODULE_INFO(mylib, 0, 1, 0);
 *     SYS_MODULE_START(mylib_start);
 *     SYS_MODULE_STOP(mylib_stop);
 *
 *     int mylib_start(size_t args, void *argp) { ... return SYS_PRX_RESIDENT; }
 *     int mylib_stop(size_t args, void *argp)  { ... return SYS_PRX_STOP_OK; }
 *
 * How each macro is realised in this toolchain:
 *
 *   SYS_MODULE_INFO(name, attribute, major, minor)
 *       Records the module name, attributes word and version in the
 *       non-loaded section ".ps3dk.module_identity".  The crt object
 *       (lv2-prx.S) owns the module-info block itself; `prx-gen build'
 *       copies these values into it, so ps3_add_prx no longer needs
 *       NAME/VERSION/ATTRIBUTES when the source declares them.  If both
 *       are given they must agree, or prx-gen refuses the module.  `name'
 *       is an identifier (it is stringized) of at most
 *       SYS_MODULE_NAME_LEN characters.  `attribute', `major' and `minor'
 *       are emitted through the assembler, so they must be integer
 *       literals or macros expanding to literal arithmetic (enumerators
 *       and const variables are not accepted).  Use it once per module.
 *
 *   SYS_MODULE_START(fn) / SYS_MODULE_STOP(fn)
 *       Make `fn' the module's module_start / module_stop: the symbol is
 *       an alias of `fn', overriding the crt's weak defaults, so the
 *       management export record points at `fn'.  `fn' must be defined
 *       in the same translation unit (the build fails otherwise), take
 *       (size_t args, void *argp) and return SYS_PRX_RESIDENT or
 *       SYS_PRX_NO_RESIDENT for start, or SYS_PRX_STOP_OK or
 *       SYS_PRX_STOP_FAIL for stop.  In C++ declare it extern "C".  Defining a
 *       function named module_start / module_stop directly (see
 *       <sys/prx_module.h>) remains equivalent.
 *
 *   SYS_MODULE_EXIT(fn), SYS_MODULE_STOP_LEVEL(level)
 *       Not supported yet: the management record built by the crt
 *       carries module_start and module_stop only, and variable exports
 *       are not supported.  Using either macro fails to compile rather
 *       than producing a module that silently ignores it.
 *
 * Each macro expands to a file-scope declaration and expects a trailing
 * semicolon.
 */

#ifndef __SYS_MODULEEXPORT_H__
#define __SYS_MODULEEXPORT_H__

#include <sys/prx_module.h>

/* Longest module name the module-info block holds (plus its NUL). */
#define SYS_MODULE_NAME_LEN 27

/* Module attributes word (SYS_MODULE_INFO's `attribute'). */
#define SYS_MODULE_ATTR_CANT_STOP       (0x0001)
#define SYS_MODULE_ATTR_EXCLUSIVE_LOAD  (0x0002)
#define SYS_MODULE_ATTR_EXCLUSIVE_START (0x0004)

/* Stop levels (SYS_MODULE_STOP_LEVEL's `level'). */
#define SYS_MODULE_STOP_LEVEL_USER      0x00000000
#define SYS_MODULE_STOP_LEVEL_SYSTEM    0x00004000

#ifndef __ASSEMBLER__

#define __PS3DK_MODEXP_STR(x)  #x
#define __PS3DK_MODEXP_XSTR(x) __PS3DK_MODEXP_STR(x)

#if defined(__cplusplus)
#define __PS3DK_MODEXP_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define __PS3DK_MODEXP_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/*
 * Identity record (36 bytes, big-endian), read by `prx-gen build':
 *   0x00 "PMI\1"   magic and record version
 *   0x04 u16       attributes
 *   0x06 u8, u8    version major, minor
 *                  prx-gen converts these to module-info minor, major
 *   0x08 char[28]  module name, NUL-terminated
 */
#define SYS_MODULE_INFO(name, attribute, major, minor)                     \
    __PS3DK_MODEXP_ASSERT(sizeof(#name) <= SYS_MODULE_NAME_LEN + 1,        \
                          "SYS_MODULE_INFO: module name is too long");     \
    __PS3DK_MODEXP_ASSERT(sizeof(#name) > 1,                               \
                          "SYS_MODULE_INFO: module name is empty");        \
    __PS3DK_MODEXP_ASSERT((attribute) >= 0 && (attribute) <= 0xffff,       \
                          "SYS_MODULE_INFO: attributes must fit 16 bits"); \
    __PS3DK_MODEXP_ASSERT((major) >= 0 && (major) <= 0xff &&               \
                          (minor) >= 0 && (minor) <= 0xff,                 \
                          "SYS_MODULE_INFO: version parts must fit 8 bits"); \
    __asm__(".pushsection .ps3dk.module_identity,\"\",@progbits\n\t"       \
            ".ascii \"PMI\\001\"\n\t"                                      \
            ".short " __PS3DK_MODEXP_XSTR(attribute) "\n\t"                \
            ".byte " __PS3DK_MODEXP_XSTR(major) ", "                       \
                     __PS3DK_MODEXP_XSTR(minor) "\n\t"                     \
            "0:\n\t"                                                       \
            ".asciz \"" #name "\"\n\t"                                     \
            ".org 0b + 28\n\t"                                             \
            ".popsection");                                                \
    extern int __ps3dk_module_info_declared

/*
 * A compiler-visible alias: GCC refuses it unless `fn' is defined in this
 * translation unit, so a misspelled or missing target cannot fall back to
 * the crt's weak default.
 */
#define __PS3DK_MODEXP_BIND(entry, fn)                                     \
    int entry(size_t args, void *argp) __attribute__((alias(#fn)))

#define SYS_MODULE_START(fn) __PS3DK_MODEXP_BIND(module_start, fn)
#define SYS_MODULE_STOP(fn)  __PS3DK_MODEXP_BIND(module_stop, fn)

#define SYS_MODULE_EXIT(fn)                                                \
    __PS3DK_MODEXP_ASSERT(0, "SYS_MODULE_EXIT is not supported yet: "      \
                          "modules carry module_start and module_stop only"); \
    extern int __ps3dk_module_exit_declared

#define SYS_MODULE_STOP_LEVEL(level)                                       \
    __PS3DK_MODEXP_ASSERT(0, "SYS_MODULE_STOP_LEVEL is not supported yet: " \
                          "modules cannot export variables");              \
    extern int __ps3dk_module_stop_level_declared

#endif /* !__ASSEMBLER__ */

#endif /* __SYS_MODULEEXPORT_H__ */

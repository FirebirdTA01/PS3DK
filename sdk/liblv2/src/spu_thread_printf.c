/*
 * liblv2 — spu_thread_printf / spu_thread_sprintf (independent).
 *
 * Behavioral contract (re-derived from the documented SPU-printf
 * protocol, not copied):
 *
 *   When an SPU thread issues a printf, it leaves the format string
 *   and its arguments in its local store and signals the PPU.  The
 *   PPU pulls those bytes out with sysSpuThreadReadLocalStorage and
 *   does the actual formatting host-side, because the SPU has no libc
 *   stdio.
 *
 *   LS argument-block layout (fixed by the SPU printf ABI):
 *     - the word at `arg_addr` holds the LS address of the format
 *       string
 *     - the call's variadic arguments start at `arg_addr` and are
 *       each stored in a 16-byte (quadword) slot; argument N lives at
 *       arg_addr + (1 + N) * 16, value right-justified in the slot.
 *
 *   This is a standalone, independent reimplementation of the C99
 *   conversion grammar limited to the conversions the SPU printf path
 *   uses: %c %s %d %i %u %o %x %X %f %%, with '-', '+', ' ', '#', '0'
 *   flags, decimal / '*' field width and precision, and the h / l / L
 *   length qualifiers.  Integer and string source bytes are fetched
 *   one access at a time from LS via sysSpuThreadReadLocalStorage.
 *
 *   The output buffer is a fixed 4 KiB scratch area (SPU printf lines
 *   are short); spu_thread_printf writes the formatted text to stdout.
 */

/* SDK headers first (they pull the legacy <ppu-asm.h>), our guarded
 * <sys/ppu-asm.h> last — see thread_wrapper.c for the rationale. */
#include <stdio.h>
#include <stddef.h>
#include <sys/spu.h>
#include <sys/spu_thread_printf.h>
#include <sys/ppu-asm.h>

#define FL_LEFT     0x01    /* '-' : left-justify            */
#define FL_PLUS     0x02    /* '+' : always show sign        */
#define FL_SPACE    0x04    /* ' ' : space before positive   */
#define FL_ALT      0x08    /* '#' : 0 / 0x prefix           */
#define FL_ZERO     0x10    /* '0' : zero-pad                */
#define FL_SIGNED   0x20    /* signed conversion             */
#define FL_UPPER    0x40    /* upper-case hex digits         */

#define OUT_CAP     4096

static char g_out[OUT_CAP] __attribute__((aligned(16)));

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Fetch one byte from SPU local store. */
static char ls_byte(sys_spu_thread_t id, u64 ls_addr)
{
    u64 v = 0;
    sysSpuThreadReadLocalStorage(id, (u32)ls_addr, &v, sizeof(char));
    return (char)v;
}

/* Fetch a sized scalar from an LS argument slot. */
static u64 ls_arg(sys_spu_thread_t id, u32 arg_addr, int idx, u32 width)
{
    u64 v = 0;
    sysSpuThreadReadLocalStorage(id,
        arg_addr + (u32)((1 + idx) * 16), &v, width);
    return v;
}

/* Parse a base-10 run starting at *p (LS address), advancing *p. */
static int ls_atoi(sys_spu_thread_t id, u64 *p)
{
    int n = 0;
    char c;
    while (is_digit((c = ls_byte(id, *p)))) {
        n = n * 10 + (c - '0');
        ++(*p);
    }
    return n;
}

/* Length of an LS string, optionally capped by `maxlen` (<0 = no cap). */
static int ls_strnlen(sys_spu_thread_t id, u32 addr, int maxlen)
{
    int n = 0;
    for (;;) {
        if (ls_byte(id, addr++) == 0)
            break;
        if (maxlen >= 0 && n >= maxlen)
            break;
        ++n;
    }
    return n;
}

/* Emit an integer, honoring base/width/precision/flags. */
static char *emit_int(char *o, u64 val, int base, int width,
                      int prec, unsigned flags)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *dig = (flags & FL_UPPER) ? up : lo;
    char tmp[32];
    int  ti = 0;
    char sign = 0;
    int  pad;

    if (flags & FL_LEFT)
        flags &= ~FL_ZERO;

    if (flags & FL_SIGNED) {
        s64 sv = (s64)val;
        if (sv < 0) { sign = '-'; val = (u64)(-sv); }
        else if (flags & FL_PLUS)  sign = '+';
        else if (flags & FL_SPACE) sign = ' ';
    }

    if (val == 0)
        tmp[ti++] = '0';
    else
        while (val) { tmp[ti++] = dig[val % (unsigned)base];
                      val /= (unsigned)base; }

    while (ti < prec)              /* precision = min digit count */
        tmp[ti++] = '0';

    pad = width - ti - (sign ? 1 : 0);
    if (flags & FL_ALT) {
        if (base == 16) pad -= 2;
        else if (base == 8) pad -= 1;
    }

    if (!(flags & (FL_LEFT | FL_ZERO)))
        while (pad-- > 0) *o++ = ' ';
    if (sign) *o++ = sign;
    if (flags & FL_ALT) {
        if (base == 8)  *o++ = '0';
        if (base == 16) { *o++ = '0'; *o++ = (flags & FL_UPPER) ? 'X' : 'x'; }
    }
    if (flags & FL_ZERO)
        while (pad-- > 0) *o++ = '0';
    while (ti-- > 0)
        *o++ = tmp[ti];
    while (pad-- > 0)               /* left-justify trailing pad */
        *o++ = ' ';
    return o;
}

/* Emit a double with fixed precision (default 6).  Plain %f only. */
static char *emit_double(char *o, double v, int width, int prec,
                         unsigned flags)
{
    static const double p10[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9
    };
    char  sign = 0;
    char  body[64];
    int   bi = 0;
    int   i, pad;
    long long whole;
    double frac;
    unsigned long long fdig;

    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;

    if (v < 0.0)              { sign = '-'; v = -v; }
    else if (flags & FL_PLUS)   sign = '+';
    else if (flags & FL_SPACE)  sign = ' ';

    whole = (long long)v;
    frac  = (v - (double)whole) * p10[prec];
    fdig  = (unsigned long long)(frac + 0.5);
    if ((double)fdig >= p10[prec]) {       /* carry on round-up */
        fdig = 0;
        whole += 1;
    }

    if (prec > 0) {
        for (i = 0; i < prec; ++i) {
            body[bi++] = (char)('0' + (int)(fdig % 10));
            fdig /= 10;
        }
        body[bi++] = '.';
    }
    do {
        body[bi++] = (char)('0' + (int)(whole % 10));
        whole /= 10;
    } while (whole);

    pad = width - bi - (sign ? 1 : 0);
    if (!(flags & (FL_LEFT | FL_ZERO)))
        while (pad-- > 0) *o++ = ' ';
    if (sign) *o++ = sign;
    if (flags & FL_ZERO)
        while (pad-- > 0) *o++ = '0';
    while (bi-- > 0)
        *o++ = body[bi];
    while (pad-- > 0)
        *o++ = ' ';
    return o;
}

s32 spu_thread_sprintf(char *buf, sys_spu_thread_t id, u32 arg_addr)
{
    u64  fmt;
    int  argn = 0;
    char *o = buf;
    char *end = buf + OUT_CAP - 1;

    /* word at arg_addr -> LS address of the format string */
    {
        u64 t = 0;
        sysSpuThreadReadLocalStorage(id, arg_addr, &t, sizeof(u32));
        fmt = t;
    }

    while (o < end) {
        char c = ls_byte(id, fmt++);
        if (c == 0)
            break;
        if (c != '%') {
            *o++ = c;
            continue;
        }

        unsigned flags = 0;
        int width = -1, prec = -1, qual = 0;

        /* flags */
        for (;;) {
            c = ls_byte(id, fmt);
            if      (c == '-') flags |= FL_LEFT;
            else if (c == '+') flags |= FL_PLUS;
            else if (c == ' ') flags |= FL_SPACE;
            else if (c == '#') flags |= FL_ALT;
            else if (c == '0') flags |= FL_ZERO;
            else break;
            ++fmt;
        }

        /* field width */
        c = ls_byte(id, fmt);
        if (is_digit(c)) {
            width = ls_atoi(id, &fmt);
        } else if (c == '*') {
            ++fmt;
            width = (int)(s32)ls_arg(id, arg_addr, argn++, sizeof(int));
            if (width < 0) { width = -width; flags |= FL_LEFT; }
        }

        /* precision */
        c = ls_byte(id, fmt);
        if (c == '.') {
            ++fmt;
            c = ls_byte(id, fmt);
            if (is_digit(c))
                prec = ls_atoi(id, &fmt);
            else if (c == '*') {
                ++fmt;
                prec = (int)(s32)ls_arg(id, arg_addr, argn++,
                                        sizeof(int));
            } else {
                prec = 0;
            }
        }

        /* length qualifier */
        c = ls_byte(id, fmt);
        if (c == 'h' || c == 'l' || c == 'L') {
            qual = c;
            ++fmt;
        }

        /* conversion */
        c = ls_byte(id, fmt);
        switch (c) {
        case 'c': {
            char ch = (char)ls_arg(id, arg_addr, argn++, sizeof(char));
            if (!(flags & FL_LEFT))
                while (--width > 0) *o++ = ' ';
            *o++ = ch;
            while (--width > 0) *o++ = ' ';
            break;
        }
        case 's': {
            u32 sa = (u32)ls_arg(id, arg_addr, argn++, sizeof(u32));
            int len, i;
            if (sa == 0) {
                static const char nil[] = "<NULL>";
                len = (int)sizeof(nil) - 1;
                if (prec >= 0 && prec < len) len = prec;
                if (!(flags & FL_LEFT))
                    while (len < width--) *o++ = ' ';
                for (i = 0; i < len; ++i) *o++ = nil[i];
                while (len < width--) *o++ = ' ';
            } else {
                len = ls_strnlen(id, sa, prec);
                if (!(flags & FL_LEFT))
                    while (len < width--) *o++ = ' ';
                for (i = 0; i < len; ++i)
                    *o++ = ls_byte(id, sa + (u32)i);
                while (len < width--) *o++ = ' ';
            }
            break;
        }
        case '%':
            *o++ = '%';
            break;
        case 'd':
        case 'i':
            flags |= FL_SIGNED;
            /* fall through */
        case 'u': {
            u64 v;
            if (qual == 'l')
                v = ls_arg(id, arg_addr, argn++, sizeof(u64));
            else if (qual == 'h') {
                v = ls_arg(id, arg_addr, argn++, sizeof(u16));
                if (flags & FL_SIGNED) v = (u64)(s64)(s16)v;
            } else if (flags & FL_SIGNED)
                v = (u64)(s64)(s32)ls_arg(id, arg_addr, argn++,
                                          sizeof(s32));
            else
                v = ls_arg(id, arg_addr, argn++, sizeof(u32));
            o = emit_int(o, v, 10, width, prec, flags);
            break;
        }
        case 'o': {
            u64 v = (qual == 'l')
                  ? ls_arg(id, arg_addr, argn++, sizeof(u64))
                  : ls_arg(id, arg_addr, argn++, sizeof(u32));
            o = emit_int(o, v, 8, width, prec, flags);
            break;
        }
        case 'X':
            flags |= FL_UPPER;
            /* fall through */
        case 'x': {
            u64 v = (qual == 'l')
                  ? ls_arg(id, arg_addr, argn++, sizeof(u64))
                  : ls_arg(id, arg_addr, argn++, sizeof(u32));
            o = emit_int(o, v, 16, width, prec, flags);
            break;
        }
        case 'f': {
            ieee64 fv;
            fv.u = ls_arg(id, arg_addr, argn++, sizeof(u64));
            o = emit_double(o, fv.d, width, prec, flags);
            break;
        }
        default:
            *o++ = '%';
            if (c) *o++ = c;
            else   --fmt;       /* trailing '%' at end of fmt */
            break;
        }
    }

    *o = '\0';
    return (s32)(o - buf);
}

s32 spu_thread_printf(sys_spu_thread_t id, u32 arg_addr)
{
    s32 n = spu_thread_sprintf(g_out, id, arg_addr);
    fwrite(g_out, 1, (size_t)n, stdout);
    return n;
}

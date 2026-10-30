/* GL ES 2.0: draw/enable entry points must raise the required error and
   leave no state change.  Each row calls the entry point, reads the first
   error (or GL_NO_ERROR for a legal no-op), and verifies it.  No scene
   setup is needed because the error checks and the legal no-op path fire
   before any state inspection or draw dispatch (see README).
   Scope (t_3093d190):
   - count < 0        -> INVALID_VALUE
   - count == 0       -> legal no-op (NOT an error)
   - unknown mode     -> INVALID_ENUM
   - bad index type   -> INVALID_ENUM, EXCEPT UNSIGNED_BYTE which stays a
                        legal no-op (GCM has no 8-bit type; separate card)
   - unknown cap      -> INVALID_ENUM, no state change
   Each row drains the error flag first and makes one call, so every row
   checks a single error; no row combines two invalid arguments. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
SYS_PROCESS_PARAM(1001, 0x100000);

static unsigned checks, failures;

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_INV_ENUM FAIL %s\n", name); }
}

/* Call fn, clear any prior error, and compare the first error against want. */
static void row(const char *name, GLenum want, void (*fn)(void))
{
    while (glGetError()) {}
    fn();
    GLenum got = glGetError();
    if (got != want)
        printf("PSGL_INV_ENUM DETAIL %s want=0x%04x got=0x%04x\n",
               name, (unsigned)want, (unsigned)got);
    check(got == want, name);
}

static void r_draw_arrays_unknown_mode(void)      { glDrawArrays(0x9999, 0, 4); }
static void r_draw_arrays_negative_first(void)    { glDrawArrays(GL_TRIANGLES, -1, 4); }
static void r_draw_arrays_negative_count(void)    { glDrawArrays(GL_TRIANGLES, 0, -1); }
static void r_draw_arrays_zero_count(void)        { glDrawArrays(GL_TRIANGLES, 0, 0); }

static void r_draw_elements_unknown_mode(void)    { glDrawElements(0x9999, 4, GL_UNSIGNED_SHORT, 0); }
static void r_draw_elements_bad_type(void)        { glDrawElements(GL_TRIANGLES, 4, (GLenum)0x9999, 0); }
static void r_draw_elements_negative_count(void)  { glDrawElements(GL_TRIANGLES, -1, GL_UNSIGNED_SHORT, 0); }
static void r_draw_elements_zero_count(void)      { glDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, 0); }
static void r_draw_elements_unsigned_byte(void)   { glDrawElements(GL_TRIANGLES, 4, GL_UNSIGNED_BYTE, 0); }
static void r_draw_elements_unsigned_byte_negative(void) { glDrawElements(GL_TRIANGLES, -1, GL_UNSIGNED_BYTE, 0); }

static void r_enable_unknown_cap(void)            { glEnable(0x9999); }
static void r_disable_unknown_cap(void)           { glDisable(0x9999); }

static int scissor_state(void)
{
    GLint value = 0;
    glGetIntegerv(GL_SCISSOR_TEST, &value);
    return value == GL_TRUE;
}

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_INV_ENUM FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    while (glGetError() != GL_NO_ERROR) {}

    row("draw_arrays unknown mode is INVALID_ENUM", GL_INVALID_ENUM, r_draw_arrays_unknown_mode);
    row("draw_arrays negative first is INVALID_VALUE", GL_INVALID_VALUE, r_draw_arrays_negative_first);
    row("draw_arrays negative count is INVALID_VALUE", GL_INVALID_VALUE, r_draw_arrays_negative_count);
    row("draw_arrays zero count is a legal no-op", GL_NO_ERROR, r_draw_arrays_zero_count);

    row("draw_elements unknown mode is INVALID_ENUM", GL_INVALID_ENUM, r_draw_elements_unknown_mode);
    row("draw_elements bad type is INVALID_ENUM", GL_INVALID_ENUM, r_draw_elements_bad_type);
    row("draw_elements negative count is INVALID_VALUE", GL_INVALID_VALUE, r_draw_elements_negative_count);
    row("draw_elements zero count is a legal no-op", GL_NO_ERROR, r_draw_elements_zero_count);
    /* UNSIGNED_BYTE is a legal ES 2.0 index type but the command set has
       no 8-bit form, so today it stays a legal no-op (no error, no draw);
       the widen-to-16 path is tracked on a separate card. */
    row("draw_elements UNSIGNED_BYTE is a legal no-op", GL_NO_ERROR, r_draw_elements_unsigned_byte);
    /* The byte no-op must not bypass count validation. */
    row("draw_elements UNSIGNED_BYTE negative count is INVALID_VALUE", GL_INVALID_VALUE,
        r_draw_elements_unsigned_byte_negative);

    /* A public cap query (GL_SCISSOR_TEST) can observe whether an unknown
       cap left a known cap untouched. */
    glEnable(GL_SCISSOR_TEST);
    check(scissor_state(), "scissor canary turned on");
    row("enable unknown cap is INVALID_ENUM", GL_INVALID_ENUM, r_enable_unknown_cap);
    check(scissor_state(), "unknown enable left a known cap on");

    glDisable(GL_SCISSOR_TEST);
    check(!scissor_state(), "scissor canary turned off");
    row("disable unknown cap is INVALID_ENUM", GL_INVALID_ENUM, r_disable_unknown_cap);
    check(!scissor_state(), "unknown disable left a known cap off");

    printf("PSGL_INV_ENUM %s checks=%u failures=%u\n",
           failures ? "FAIL" : "OK", checks, failures);
    psglExit();
    return failures ? 1 : 0;
}

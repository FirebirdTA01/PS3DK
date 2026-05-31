#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

static int nearly(GLfloat a, GLfloat b)
{
    GLfloat d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.0002f;
}

static int check4(const char *name, const GLfloat *got, const GLfloat *want)
{
    for (int i = 0; i < 4; i++) {
        if (!nearly(got[i], want[i])) {
            printf("hello-psgl-light-state: %s[%d] got=%f want=%f\n",
                   name, i, got[i], want[i]);
            return 0;
        }
    }
    return 1;
}

static int check1(const char *name, GLfloat got, GLfloat want)
{
    if (!nearly(got, want)) {
        printf("hello-psgl-light-state: %s got=%f want=%f\n",
               name, got, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    PSGLinitOptions options = {0};
    GLfloat value4[4];
    GLfloat ambient[4] = {0.10f, 0.20f, 0.30f, 1.00f};
    GLfloat model_ambient[4] = {0.25f, 0.30f, 0.35f, 1.00f};
    GLfloat diffuse[4] = {0.40f, 0.50f, 0.60f, 1.00f};
    GLfloat color[4] = {0.70f, 0.80f, 0.90f, 1.00f};
    GLboolean enabled = GL_FALSE;
    int ok = 1;

    psglInit(&options);
    if (!psglGetCurrentContext()) {
        printf("hello-psgl-light-state: no context\n");
        psglExit();
        return 1;
    }

    glLightfv(GL_LIGHT2, GL_AMBIENT, ambient);
    glGetLightfv(GL_LIGHT2, GL_AMBIENT, value4);
    ok = ok && check4("light2 ambient", value4, ambient);

    glLightf(GL_LIGHT2, GL_SPOT_CUTOFF, 45.0f);
    glGetLightfv(GL_LIGHT2, GL_SPOT_CUTOFF, value4);
    ok = ok && check1("light2 cutoff", value4[0], 45.0f);

    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, model_ambient);
    glGetFloatv(GL_LIGHT_MODEL_AMBIENT, value4);
    ok = ok && check4("model ambient", value4, model_ambient);

    glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, 1.0f);
    glGetFloatv(GL_LIGHT_MODEL_TWO_SIDE, value4);
    ok = ok && check1("two side", value4[0], 1.0f);

    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, value4);
    ok = ok && check4("front diffuse", value4, diffuse);
    glGetMaterialfv(GL_BACK, GL_DIFFUSE, value4);
    ok = ok && check4("back diffuse", value4, diffuse);

    glMaterialf(GL_FRONT, GL_SHININESS, 32.0f);
    glGetMaterialfv(GL_FRONT, GL_SHININESS, value4);
    ok = ok && check1("front shininess", value4[0], 32.0f);

    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glColor4f(color[0], color[1], color[2], color[3]);
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, value4);
    ok = ok && check4("color material ambient", value4, color);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, value4);
    ok = ok && check4("color material diffuse", value4, color);

    glShadeModel(GL_FLAT);
    glGetFloatv(GL_SHADE_MODEL, value4);
    ok = ok && check1("shade model", value4[0], (GLfloat)GL_FLAT);

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT2);
    glGetBooleanv(GL_LIGHTING, &enabled);
    ok = ok && (enabled == GL_TRUE);
    glGetBooleanv(GL_LIGHT2, &enabled);
    ok = ok && (enabled == GL_TRUE);

    printf("hello-psgl-light-state: %s\n", ok ? "ok" : "fail");
    psglExit();
    return ok ? 0 : 1;
}

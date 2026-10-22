/* OB-E1 / OB-E2 (erratum 1 E4, erratum 2 F4): ONE valid caller source, compiled unchanged as C and as C++, with
 * PS3DK_PROF=1 (linked to the host library), with PS3DK_PROF undefined and with PS3DK_PROF=0.
 *
 * The zone names are string literals (they initialise a static descriptor when enabled), so the side effects sit in
 * the runtime arguments: a token expression, a handle expression and a thread name.  Enabled, each runtime argument is
 * evaluated exactly once per call (hits == 4); disabled, none is evaluated (hits == 0) and no function is referenced.
 * Every sizeof operand here is fixed-size (F4): a string literal, a ps3dk_prof_token, a ps3dk_prof_zone lvalue. */
#include <ps3dk/prof.h>

#include <stdio.h>
#include <string.h>

#if defined(PS3DK_PROF) && PS3DK_PROF
#define OB_ENABLED 1
#define OB_EXPECT_HITS 4
#else
#define OB_ENABLED 0
#define OB_EXPECT_HITS 0
#endif

int hits;
const char *names[4] = {"main", "main", "main", "main"};

ps3dk_prof_token next_token(int *h);
ps3dk_prof_token next_token(int *h)
{
    (*h)++;
    return PS3DK_PROF_TOKEN_NONE;
}

void body(void);
void body(void)
{
    ps3dk_prof_zone zs[2];
    ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("a");
    zs[0] = PS3DK_PROF_ZONE_BEGIN("a0");
    zs[1] = PS3DK_PROF_ZONE_BEGIN("a1");
    PS3DK_PROF_ZONE_END(zs[1 - hits++]);   /* the array form: zs[1], then zs[0] */
    PS3DK_PROF_ZONE_END(zs[1 - hits++]);
    {
        PS3DK_PROF_ZONE("scoped");
        {
            PS3DK_PROF_ZONE_FOR("b", next_token(&hits));
        }
    }
    PS3DK_PROF_ZONE_END(z);
}

int main(int argc, char **argv)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_token tok;
    int rc;
    memset(&cfg, 0, sizeof cfg);
#if OB_ENABLED
    if (argc != 2)
        return 2;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "ob";
    if (ps3dk_prof_set_sink_path(argv[1]) != PS3DK_PROF_OK)
        return 3;
#else
    (void)argc;
    (void)argv;
#endif
    rc = PS3DK_PROF_INIT(&cfg);
    if (rc != PS3DK_PROF_OK)
        return 4;
    rc = PS3DK_PROF_THREAD_BEGIN(names[hits++]);
    if (rc != PS3DK_PROF_OK)
        return 5;
    PS3DK_PROF_FRAME();
    tok = PS3DK_PROF_FRAME_TOKEN();
    {
        ps3dk_prof_zone t = PS3DK_PROF_ZONE_BEGIN_FOR("t", tok);   /* a token zone */
        PS3DK_PROF_ZONE_END(t);
    }
    body();
    rc = PS3DK_PROF_DRAIN();
    if (rc != PS3DK_PROF_OK)
        return 6;
    PS3DK_PROF_THREAD_END();
    rc = PS3DK_PROF_SHUTDOWN();
    printf("hits=%d\n", hits);
    if (hits != OB_EXPECT_HITS)
        return 7;
    return rc == PS3DK_PROF_OK ? 0 : 8;
}

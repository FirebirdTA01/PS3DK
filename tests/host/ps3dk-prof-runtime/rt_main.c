/* The host B test registry: `rt --list`, or `rt <test> <out-dir>` (one test per process). */
#include "rt.h"

#include <string.h>

#define RT(name) int name(const char *out);
#include "rt_list.h"
#undef RT

static const struct {
    const char *name;
    int (*fn)(const char *out);
} tests[] = {
#define RT(name) {#name, name},
#include "rt_list.h"
#undef RT
};

int main(int argc, char **argv)
{
    size_t i;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (i = 0; i < sizeof tests / sizeof tests[0]; i++)
            printf("%s\n", tests[i].name);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s --list | %s <test> <out-dir>\n", argv[0], argv[0]);
        return 2;
    }
    for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (strcmp(argv[1], tests[i].name) == 0) {
            int rc = tests[i].fn(argv[2]);
            if (rc == 0 && rt_worker_failed())
                rc = 1;
            fprintf(stderr, "%s: %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
            return rc == 0 ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown test %s\n", argv[1]);
    return 2;
}

/*
 * psgl-shader-builder: compatibility wrapper, kept for one release.
 *
 * The fixed-function shader library is now built by
 *   rsx-cg-compiler --psgl-ffp-library <state.txt> -o <shaders.bin>
 * which compiles every mask in process.  This program keeps the old command
 * line (compiler first, an optional work directory last, which is no longer
 * used), runs that mode and exits with its status.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Documented unversioned development default outside packaging / formal builds */
#ifndef PSGL_SHADER_BUILDER_VERSION
#define PSGL_SHADER_BUILDER_VERSION "dev"
#endif

static int run_ffp_library(const char *compiler, const char *state_path,
                           const char *out_path)
{
    const char *const argv[] = {
        compiler,
        "--psgl-ffp-library",
        state_path,
        "-o",
        out_path,
        NULL,
    };
#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, compiler, argv);
    if (rc == -1) {
        perror(compiler);
        return 1;
    }
    return (int)rc;
#else
    int status = 0;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execvp(compiler, (char *const *)argv);
        perror(compiler);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#endif
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("psgl-shader-builder %s\n", PSGL_SHADER_BUILDER_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: %s <rsx-cg-compiler> <state.txt> <shaders.bin> [work-dir]\n"
               "deprecated: runs <rsx-cg-compiler> --psgl-ffp-library <state.txt> -o <shaders.bin>\n",
               argv[0]);
        return 0;
    }

    if (argc != 4 && argc != 5) {
        fprintf(stderr,
                "usage: %s <rsx-cg-compiler> <state.txt> <shaders.bin> [work-dir]\n",
                argv[0]);
        return 2;
    }

    fprintf(stderr,
            "psgl-shader-builder: deprecated, use: rsx-cg-compiler --psgl-ffp-library %s -o %s\n",
            argv[2], argv[3]);
    return run_ffp_library(argv[1], argv[2], argv[3]);
}

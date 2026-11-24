// ps3dk-run test child: exercises the loader acceptance list.
//
//   (no option)      argv dump, side file, network, stdout+stderr, teardown output, exit code 7
//   --spin           spins forever on the main thread (watchdog: TIMEOUT + exitspawn from a second thread)
//   --flood N        writes N lines of 1 KiB quickly (backpressure / dropped-byte accounting)
//   --exit-code N    exits with N
#include "../../hook/ps3dk_run_hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <net/net.h>
#include <netinet/in.h>
#include <sys/process.h>
#include <sys/socket.h>

SYS_PROCESS_PARAM(1001, 0x100000);

/* CRUMB child (launch-reliability arm A2): two breadcrumbs that split the launch window - one constructor at
 * priority 82 (before the heap at 83, newlib's locks and every library constructor) and one at the top of main.
 * Each appends one line "<timebase hex> <label>" to /dev_hdd0/tmp/ps3dk-child-crumb.log with inline lv2 FS
 * syscalls only: no libc, no heap, no locks, no PRX imports; the time is the timebase register. */
#include <sys/file.h>

static void child_crumb(const char *label)
{
    char line[64];
    unsigned long long tb;
    int n = 0;
    s32 fd;
    u64 w;
    __asm__ volatile("mftb %0" : "=r"(tb));
    for (int s = 60; s >= 0; s -= 4) line[n++] = "0123456789abcdef"[(tb >> s) & 15];
    line[n++] = ' ';
    while (*label && n < (int)sizeof line - 1) line[n++] = *label++;
    line[n++] = 10;
    if (sysLv2FsOpen("/dev_hdd0/tmp/ps3dk-child-crumb.log", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) != 0) return;
    sysLv2FsWrite(fd, line, (u64)n, &w);
    sysLv2FsClose(fd);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
__attribute__((constructor(82))) static void child_crumb_ctor(void) { child_crumb("crumb: constructor 82"); }
#pragma GCC diagnostic pop

namespace {
struct Teardown {
    ~Teardown() { std::printf("CHILD teardown: static destructor ran\n"); }
} g_teardown;

void at_exit_handler() { std::printf("CHILD teardown: atexit handler ran\n"); }

std::string dir_of(const char *path)
{
    std::string p = path ? path : "";
    auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}
}

int main(int argc, const char *argv[])
{
    child_crumb("crumb: main entered");
    int hooked = ps3dk_run_hook_start(&argc, &argv);
    std::atexit(at_exit_handler);
    std::printf("CHILD hook=%d argc=%d\n", hooked, argc);
    for (int i = 0; i < argc; i++) {
        if (argv[i]) std::printf("CHILD argv[%d]=[%s] len=%zu\n", i, argv[i], std::strlen(argv[i]));
        else std::printf("CHILD argv[%d]=NULL\n", i);
    }

    int exit_code = 7;
    bool spin = false;
    long flood = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) continue;
        if (!std::strcmp(argv[i], "--spin")) spin = true;
        else if (!std::strcmp(argv[i], "--flood") && i + 1 < argc) flood = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--exit-code") && i + 1 < argc) exit_code = std::atoi(argv[++i]);
    }

    // side file next to the SELF (acceptance 2): argv[0] when it is a path, else the run directory is unknown
    std::string side = dir_of(argc > 0 ? argv[0] : nullptr) + "/data/side.txt";
    if (FILE *f = std::fopen(side.c_str(), "rb")) {
        char buf[128] = {};
        size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        std::printf("CHILD side file %s: %zu bytes [%s]\n", side.c_str(), n, buf);
    } else {
        std::fprintf(stderr, "CHILD side file %s: NOT FOUND\n", side.c_str());
    }

    // the child's own network use (acceptance 3): an independent socket, independent of the hook's connection
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    std::printf("CHILD own socket: fd=%d\n", s);
    if (s >= 0) socketclose(s);

    std::fprintf(stderr, "CHILD this line is on stderr\n");

    if (flood > 0) {
        static char line[1025];
        std::memset(line, 'x', 1023);
        line[1023] = '\n';
        for (long i = 0; i < flood; i++) std::fwrite(line, 1, 1024, stdout);
        std::fflush(stdout);
        std::printf("CHILD flood of %ld KiB done\n", flood);
    }
    if (spin) {
        std::printf("CHILD spinning\n");
        std::fflush(stdout);
        static volatile unsigned long spins;
        for (;;) spins = spins + 1;
    }
    std::printf("CHILD returning %d\n", exit_code);
    return exit_code;
}

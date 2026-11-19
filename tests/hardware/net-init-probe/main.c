/* Measures the firmware answers libnet's shared-network holders depend on (sdk/libnet/src/net_hold.h): what a second
 * sys_net_initialize_network_ex returns while the network is up, what a finalize returns while it is down, and
 * whether the NET sysmodule load is counted (load twice, unload once: still loaded?).  No GPU, no profiler.
 *
 * Build it against an SDK whose libnet still exports the RAW finalize as sys_net_finalize_network (v0.27.x and
 * earlier): it calls the raw PRX entry points directly, so the holder layer under test never answers in their place.
 *
 * When the network is already up at entry (the ps3dk-run hook brought it up from main before this program's main),
 * the first init answers the double-init question and every step that could tear the network or the module down is
 * skipped, so the hook's EXIT still reaches the PC.
 *
 * Output: one "NETPROBE key=value" line per measurement on the TTY, the same lines in
 * /dev_hdd0/tmp/ps3dk-netprobe.txt, and "NETPROBE done" last. */
#include <cell/libnet.h>
#include <cell/sysmodule.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct wire_init {
    uint32_t memory, size;
    int32_t flags;
};
/* The raw PRX entry points (sdk/libnet/raw-symbols.txt in the SDK this is built against). */
extern int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *);
#define raw_finalize sys_net_finalize_network

static unsigned char pool_a[128 * 1024] __attribute__((aligned(128)));
static unsigned char pool_b[128 * 1024] __attribute__((aligned(128)));
static FILE *out;

static void put(const char *key, int value)
{
    printf("NETPROBE %s=0x%08x (%d)\n", key, (unsigned)value, value);
    if (out)
        fprintf(out, "%s=0x%08x\n", key, (unsigned)value);
}

static int raw_init(unsigned char *pool)
{
    struct wire_init w = {(uint32_t)(uintptr_t)pool, 128u * 1024u, 0};
    return __ps3dk_raw_sys_net_initialize_network_ex(&w);
}

/* A listener on every interface, the way the live agent makes one. */
static void socket_check(const char *tag)
{
    char key[48];
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    snprintf(key, sizeof key, "%s_socket", tag);
    put(key, fd);
    if (fd < 0)
        return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(31759);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    snprintf(key, sizeof key, "%s_bind", tag);
    put(key, bind(fd, (struct sockaddr *)&sa, sizeof sa));
    snprintf(key, sizeof key, "%s_listen", tag);
    put(key, listen(fd, 2));
    snprintf(key, sizeof key, "%s_close", tag);
    put(key, close(fd));
}

int main(void)
{
    out = fopen("/dev_hdd0/tmp/ps3dk-netprobe.txt", "w");
    puts("NETPROBE begin");
    put("isloaded_at_entry", cellSysmoduleIsLoaded(CELL_SYSMODULE_NET));
    put("load_1", cellSysmoduleLoadModule(CELL_SYSMODULE_NET));
    put("isloaded_after_load_1", cellSysmoduleIsLoaded(CELL_SYSMODULE_NET));
    int init_1 = raw_init(pool_a);
    put("init_1", init_1);
    put("init_2_while_up", raw_init(pool_b)); /* the double-init answer */
    socket_check("up");
    if (init_1 != 0) {
        /* someone else (the ps3dk-run hook) holds the network: never tear it or the module down */
        put("held_elsewhere", 1);
    } else {
        put("held_elsewhere", 0);
        put("finalize_1", raw_finalize());
        put("finalize_2_while_down", raw_finalize()); /* the not-initialized answer */
        put("init_3_after_finalize", raw_init(pool_b));
        socket_check("reinit");
        put("finalize_3", raw_finalize());
        /* is the module load counted? */
        put("load_2_while_loaded", cellSysmoduleLoadModule(CELL_SYSMODULE_NET));
        put("unload_1", cellSysmoduleUnloadModule(CELL_SYSMODULE_NET));
        put("isloaded_after_unload_1", cellSysmoduleIsLoaded(CELL_SYSMODULE_NET));
        put("unload_2", cellSysmoduleUnloadModule(CELL_SYSMODULE_NET));
        put("isloaded_after_unload_2", cellSysmoduleIsLoaded(CELL_SYSMODULE_NET));
        put("unload_3_while_unloaded", cellSysmoduleUnloadModule(CELL_SYSMODULE_NET));
    }
    puts("NETPROBE done");
    if (out) {
        fputs("done\n", out);
        fclose(out);
    }
    return 0;
}

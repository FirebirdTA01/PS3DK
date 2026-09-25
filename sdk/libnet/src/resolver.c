/* SPDX-License-Identifier: BSD-2-Clause */
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../runtime/lv2/librt/net_errno.h"

/* PRX returns 32-bit EAs regardless of the caller's native pointer ABI. */
extern uint32_t __ps3dk_raw_gethostbyname(const char *);
extern uint32_t __ps3dk_raw_gethostbyaddr(const char *, socklen_t, int);
extern int *_sys_net_errno_loc(void);
struct wire_host { uint32_t name, aliases; int32_t type, length; uint32_t addresses; };
_Static_assert(sizeof(struct wire_host) == 20, "firmware hostent");

int h_errno;
struct resolver_state {
    struct hostent host;
    void *storage;
    struct servent service;
    char *empty_aliases[1];
    uint16_t alias_sizes[1024];
};
static pthread_once_t state_once = PTHREAD_ONCE_INIT;
static pthread_key_t state_key;
static int state_error;
static void destroy_state(void *p)
{ struct resolver_state *s = p; if (s) { free(s->storage); free(s); } }
static void create_key(void) { state_error = pthread_key_create(&state_key, destroy_state); }
static struct resolver_state *get_state(void)
{
    struct resolver_state *s;
    int e = pthread_once(&state_once, create_key);
    if (!e) e = state_error;
    if (e) { errno = e; h_errno = NETDB_INTERNAL; return NULL; }
    s = pthread_getspecific(state_key);
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) { h_errno = NETDB_INTERNAL; return NULL; }
        e = pthread_setspecific(state_key, s);
        if (e) { free(s); errno = e; h_errno = NETDB_INTERNAL; return NULL; }
    }
    return s;
}
static size_t bounded_string(const char *p)
{
    size_t n;
    if (!p) return 0;
    for (n = 0; n < 1024; ++n) if (!p[n]) return n + 1;
    return 0;
}
static int list_length(const uint32_t *p)
{
    int n;
    if (!p) return 0;
    for (n = 0; n < 1024; ++n) if (!p[n]) return n;
    return -1;
}
static struct hostent *convert_host(uint32_t address)
{
    struct resolver_state *s = get_state();
    const struct wire_host *w = (const void *)(uintptr_t)address;
    const uint32_t *aliases, *addresses;
    const char *name;
    char **alias_out, **addr_out, *cursor;
    size_t name_size, bytes;
    void *storage;
    int na, nd, i;
    if (!s) return NULL;
    free(s->storage); s->storage = NULL; memset(&s->host, 0, sizeof(s->host));
    if (!w) {
        int *h = _sys_net_h_errno_loc(), *e = _sys_net_errno_loc();
        h_errno = h && *h ? *h : NO_RECOVERY;
        if (e && *e) errno = net_error(*e);
        return NULL;
    }
    aliases = (const void *)(uintptr_t)w->aliases;
    addresses = (const void *)(uintptr_t)w->addresses;
    name = (const void *)(uintptr_t)w->name;
    na = list_length(aliases); nd = list_length(addresses); name_size = bounded_string(name);
    if (na < 0 || nd < 0 || !name_size || w->length <= 0 || w->length > 16) goto malformed;
    bytes = ((size_t)na + nd + 2) * sizeof(char *) + name_size + (size_t)nd * w->length;
    for (i = 0; i < na; ++i) {
        s->alias_sizes[i] = bounded_string((const char *)(uintptr_t)aliases[i]);
        if (!s->alias_sizes[i]) goto malformed;
        bytes += s->alias_sizes[i];
    }
    storage = malloc(bytes);
    if (!storage) { errno = ENOMEM; h_errno = NETDB_INTERNAL; return NULL; }
    alias_out = storage; addr_out = alias_out + na + 1;
    cursor = (char *)(addr_out + nd + 1);
    s->host.h_name = cursor; memcpy(cursor, name, name_size); cursor += name_size;
    for (i = 0; i < na; ++i) {
        alias_out[i] = cursor;
        memcpy(cursor, (const void *)(uintptr_t)aliases[i], s->alias_sizes[i]); cursor += s->alias_sizes[i];
    }
    alias_out[na] = NULL;
    for (i = 0; i < nd; ++i) {
        addr_out[i] = cursor;
        memcpy(cursor, (const void *)(uintptr_t)addresses[i], w->length); cursor += w->length;
    }
    addr_out[nd] = NULL;
    s->storage = storage; s->host.h_aliases = alias_out; s->host.h_addr_list = addr_out;
    s->host.h_addrtype = w->type; s->host.h_length = w->length;
    h_errno = NETDB_SUCCESS;
    return &s->host;
malformed:
    errno = EOVERFLOW; h_errno = NO_RECOVERY; return NULL;
}
struct hostent *gethostbyname(const char *name)
{
    if (!name) { errno = EINVAL; h_errno = NO_RECOVERY; return NULL; }
    return convert_host(__ps3dk_raw_gethostbyname(name));
}
struct hostent *gethostbyaddr(const char *address, socklen_t len, int type)
{
    if (!address) { errno = EINVAL; h_errno = NO_RECOVERY; return NULL; }
    return convert_host(__ps3dk_raw_gethostbyaddr(address, len, type));
}

static const struct service_entry { const char *name, *protocol; unsigned short port; } services[] = {
    {"ftp-data", "tcp", 20}, {"ftp", "tcp", 21}, {"ssh", "tcp", 22},
    {"telnet", "tcp", 23}, {"smtp", "tcp", 25}, {"domain", "tcp", 53},
    {"domain", "udp", 53}, {"http", "tcp", 80}, {"pop3", "tcp", 110},
    {"ntp", "udp", 123}, {"imap", "tcp", 143}, {"https", "tcp", 443},
    {"submission", "tcp", 587}, {"imaps", "tcp", 993}, {"pop3s", "tcp", 995}
};
static struct servent *find_service(const char *name, int port, const char *protocol)
{
    unsigned i;
    struct resolver_state *s = get_state();
    if (!s) return NULL;
    for (i = 0; i < sizeof(services)/sizeof(services[0]); ++i) {
        const struct service_entry *entry = &services[i];
        if (protocol && strcmp(protocol, entry->protocol)) continue;
        if (name ? strcmp(name, entry->name) != 0 : port != (int)htons(entry->port)) continue;
        s->service.s_name = (char *)entry->name; s->service.s_proto = (char *)entry->protocol;
        s->service.s_port = htons(entry->port); s->service.s_aliases = s->empty_aliases;
        return &s->service;
    }
    return NULL;
}
struct servent *getservbyport(int port, const char *protocol)
{ return find_service(NULL, port, protocol); }
struct servent *getservbyname(const char *name, const char *protocol)
{ if (!name) { errno = EINVAL; return NULL; } return find_service(name, 0, protocol); }

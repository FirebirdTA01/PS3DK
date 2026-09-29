/*
 * tests/sdk/lwmutex-synchronization-test.c
 *
 * Verifies sys_lwmutex and sys_lwcond in <sys/synchronization.h> across
 * the canonical layouts and PSL1GHT compatibility aliases.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/synchronization.h>

#if defined(__cplusplus)
static_assert(sizeof(sys_lwmutex_lock_info_t) == 8, "sizeof sys_lwmutex_lock_info_t must be 8");
static_assert(offsetof(sys_lwmutex_lock_info_t, owner) == 0, "offset of owner must be 0");
static_assert(offsetof(sys_lwmutex_lock_info_t, waiter) == 4, "offset of waiter must be 4");

static_assert(sizeof(sys_lwmutex_variable_t) == 8, "sizeof sys_lwmutex_variable_t must be 8");

static_assert(sizeof(sys_lwmutex_attribute_t) == 16, "sizeof sys_lwmutex_attribute_t must be 16");
static_assert(offsetof(sys_lwmutex_attribute_t, attr_protocol) == 0, "offset of attr_protocol must be 0");
static_assert(offsetof(sys_lwmutex_attribute_t, attr_recursive) == 4, "offset of attr_recursive must be 4");
static_assert(offsetof(sys_lwmutex_attribute_t, name) == 8, "offset of name must be 8");

static_assert(sizeof(sys_lwmutex_t) == 24, "sizeof sys_lwmutex_t must be 24");
static_assert(offsetof(sys_lwmutex_t, lock_var) == 0, "offset of lock_var must be 0");
static_assert(offsetof(sys_lwmutex_t, attribute) == 8, "offset of attribute must be 8");
static_assert(offsetof(sys_lwmutex_t, recursive_count) == 12, "offset of recursive_count must be 12");
static_assert(offsetof(sys_lwmutex_t, sleep_queue) == 16, "offset of sleep_queue must be 16");
static_assert(offsetof(sys_lwmutex_t, pad) == 20, "offset of pad must be 20");
static_assert(offsetof(sys_lwmutex_t, _pad) == 20, "offset of _pad must be 20");

static_assert(sizeof(sys_lwcond_attribute_t) == 8, "sizeof sys_lwcond_attribute_t must be 8");
static_assert(offsetof(sys_lwcond_attribute_t, name) == 0, "offset of name must be 0");

static_assert(sizeof(sys_lwcond_t) == (sizeof(void*) == 4 ? 8 : 16), "sizeof sys_lwcond_t");
static_assert(offsetof(sys_lwcond_t, lwmutex) == 0, "offset of lwmutex must be 0");
static_assert(offsetof(sys_lwcond_t, lwcond_queue) == sizeof(void*), "offset of lwcond_queue must follow pointer");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(sys_lwmutex_lock_info_t) == 8, "sizeof sys_lwmutex_lock_info_t must be 8");
_Static_assert(offsetof(sys_lwmutex_lock_info_t, owner) == 0, "offset of owner must be 0");
_Static_assert(offsetof(sys_lwmutex_lock_info_t, waiter) == 4, "offset of waiter must be 4");

_Static_assert(sizeof(sys_lwmutex_variable_t) == 8, "sizeof sys_lwmutex_variable_t must be 8");

_Static_assert(sizeof(sys_lwmutex_attribute_t) == 16, "sizeof sys_lwmutex_attribute_t must be 16");
_Static_assert(offsetof(sys_lwmutex_attribute_t, attr_protocol) == 0, "offset of attr_protocol must be 0");
_Static_assert(offsetof(sys_lwmutex_attribute_t, attr_recursive) == 4, "offset of attr_recursive must be 4");
_Static_assert(offsetof(sys_lwmutex_attribute_t, name) == 8, "offset of name must be 8");

_Static_assert(sizeof(sys_lwmutex_t) == 24, "sizeof sys_lwmutex_t must be 24");
_Static_assert(offsetof(sys_lwmutex_t, lock_var) == 0, "offset of lock_var must be 0");
_Static_assert(offsetof(sys_lwmutex_t, attribute) == 8, "offset of attribute must be 8");
_Static_assert(offsetof(sys_lwmutex_t, recursive_count) == 12, "offset of recursive_count must be 12");
_Static_assert(offsetof(sys_lwmutex_t, sleep_queue) == 16, "offset of sleep_queue must be 16");
_Static_assert(offsetof(sys_lwmutex_t, pad) == 20, "offset of pad must be 20");
_Static_assert(offsetof(sys_lwmutex_t, _pad) == 20, "offset of _pad must be 20");

_Static_assert(sizeof(sys_lwcond_attribute_t) == 8, "sizeof sys_lwcond_attribute_t must be 8");
_Static_assert(offsetof(sys_lwcond_attribute_t, name) == 0, "offset of name must be 0");

_Static_assert(sizeof(sys_lwcond_t) == (sizeof(void*) == 4 ? 8 : 16), "sizeof sys_lwcond_t");
_Static_assert(offsetof(sys_lwcond_t, lwmutex) == 0, "offset of lwmutex must be 0");
_Static_assert(offsetof(sys_lwcond_t, lwcond_queue) == sizeof(void*), "offset of lwcond_queue must follow pointer");
#else
typedef char assert_sizeof_lwm_lock_info[(sizeof(sys_lwmutex_lock_info_t) == 8) ? 1 : -1];
typedef char assert_sizeof_lwm_var[(sizeof(sys_lwmutex_variable_t) == 8) ? 1 : -1];

typedef char assert_sizeof_lwmutex_attr[(sizeof(sys_lwmutex_attribute_t) == 16) ? 1 : -1];
typedef char assert_off_attr_proto[(offsetof(sys_lwmutex_attribute_t, attr_protocol) == 0) ? 1 : -1];
typedef char assert_off_attr_rec[(offsetof(sys_lwmutex_attribute_t, attr_recursive) == 4) ? 1 : -1];
typedef char assert_off_attr_name[(offsetof(sys_lwmutex_attribute_t, name) == 8) ? 1 : -1];

typedef char assert_sizeof_lwmutex[(sizeof(sys_lwmutex_t) == 24) ? 1 : -1];
typedef char assert_off_lwm_lock[(offsetof(sys_lwmutex_t, lock_var) == 0) ? 1 : -1];
typedef char assert_off_lwm_attr[(offsetof(sys_lwmutex_t, attribute) == 8) ? 1 : -1];
typedef char assert_off_lwm_rec[(offsetof(sys_lwmutex_t, recursive_count) == 12) ? 1 : -1];
typedef char assert_off_lwm_sq[(offsetof(sys_lwmutex_t, sleep_queue) == 16) ? 1 : -1];
typedef char assert_off_lwm_pad[(offsetof(sys_lwmutex_t, pad) == 20) ? 1 : -1];
typedef char assert_off_lwm__pad[(offsetof(sys_lwmutex_t, _pad) == 20) ? 1 : -1];

typedef char assert_sizeof_lwcond_attr[(sizeof(sys_lwcond_attribute_t) == 8) ? 1 : -1];
typedef char assert_off_lwc_name[(offsetof(sys_lwcond_attribute_t, name) == 0) ? 1 : -1];

typedef char assert_sizeof_lwcond[(sizeof(sys_lwcond_t) == (sizeof(void*) == 4 ? 8 : 16)) ? 1 : -1];
typedef char assert_off_lwc_lwm[(offsetof(sys_lwcond_t, lwmutex) == 0) ? 1 : -1];
typedef char assert_off_lwc_q[(offsetof(sys_lwcond_t, lwcond_queue) == sizeof(void*)) ? 1 : -1];
#endif

static void fill_bytes(void *p, unsigned char val, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++) b[i] = val;
}

static int check_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void test_all_11_prototypes(void)
{
    /* Typed function pointers pin all 11 prototypes in C and C++ */
    int (* volatile p_create)(sys_lwmutex_t *, sys_lwmutex_attribute_t *) = sys_lwmutex_create;
    int (* volatile p_destroy)(sys_lwmutex_t *) = sys_lwmutex_destroy;
    int (* volatile p_lock)(sys_lwmutex_t *, usecond_t) = sys_lwmutex_lock;
    int (* volatile p_trylock)(sys_lwmutex_t *) = sys_lwmutex_trylock;
    int (* volatile p_unlock)(sys_lwmutex_t *) = sys_lwmutex_unlock;

    int (* volatile p_c_create)(sys_lwcond_t *, sys_lwmutex_t *, sys_lwcond_attribute_t *) = sys_lwcond_create;
    int (* volatile p_c_destroy)(sys_lwcond_t *) = sys_lwcond_destroy;
    int (* volatile p_c_wait)(sys_lwcond_t *, usecond_t) = sys_lwcond_wait;
    int (* volatile p_c_sig)(sys_lwcond_t *) = sys_lwcond_signal;
    int (* volatile p_c_sigall)(sys_lwcond_t *) = sys_lwcond_signal_all;
    int (* volatile p_c_sigto)(sys_lwcond_t *, sys_ppu_thread_t) = sys_lwcond_signal_to;

    (void)p_create;
    (void)p_destroy;
    (void)p_lock;
    (void)p_trylock;
    (void)p_unlock;
    (void)p_c_create;
    (void)p_c_destroy;
    (void)p_c_wait;
    (void)p_c_sig;
    (void)p_c_sigall;
    (void)p_c_sigto;

    /* Verify canonical reference struct tags in C and C++ */
    struct lwmutex_attr * volatile p_tag_attr = (sys_lwmutex_attribute_t *)0;
    struct sys_lwcond_attribute * volatile p_tag_cattr = (sys_lwcond_attribute_t *)0;
    struct sys_lwmutex * volatile p_tag_m = (sys_lwmutex_t *)0;
    struct sys_lwcond * volatile p_tag_c = (sys_lwcond_t *)0;
    (void)p_tag_attr;
    (void)p_tag_cattr;
    (void)p_tag_m;
    (void)p_tag_c;
}

int main(void)
{
    test_all_11_prototypes();

    /* 1. Lwmutex attribute initialization and name setting */
    sys_lwmutex_attribute_t lwm_attr;
    fill_bytes(&lwm_attr, 0xAA, sizeof(lwm_attr));
    sys_lwmutex_attribute_initialize(lwm_attr);
    if (lwm_attr.attr_protocol != SYS_SYNC_PRIORITY) return 1;
    if (lwm_attr.attr_recursive != SYS_SYNC_NOT_RECURSIVE) return 2;
    if (lwm_attr.name[0] != '\0') return 3;

    sys_lwmutex_attribute_name_set(lwm_attr.name, "my_lwm");
    if (!check_streq(lwm_attr.name, "my_lwm")) return 4;

    /* 2. Lwcond attribute initialization and name setting */
    sys_lwcond_attribute_t lwc_attr;
    fill_bytes(&lwc_attr, 0xBB, sizeof(lwc_attr));
    sys_lwcond_attribute_initialize(lwc_attr);
    if (lwc_attr.name[0] != '\0') return 5;

    sys_lwcond_attribute_name_set(lwc_attr.name, "my_cond");
    if (!check_streq(lwc_attr.name, "my_cond")) return 6;

    /* 3. Lwmutex structure reference members (lock_var union, pad/_pad) */
    sys_lwmutex_t lwm;
    fill_bytes(&lwm, 0, sizeof(lwm));
    lwm.lock_var.all_info = 0x1111222233334444ULL;
    if (lwm.lock_var.info.owner != 0x11112222U || lwm.lock_var.info.waiter != 0x33334444U)
        return 7;

    lwm.pad = 0xA5A5A5A5U;
    if (lwm._pad != 0xA5A5A5A5U) return 8;

    /* 4. Lwcond structure instance */
    sys_lwcond_t lwc;
    fill_bytes(&lwc, 0, sizeof(lwc));
    lwc.lwmutex = &lwm;
    if (lwc.lwmutex != &lwm) return 9;

    /* 5. Compatibility alias: sys_lwmutex_attr_t and PSL1GHT forwarders */
    sys_lwmutex_attr_t compat_attr;
    sys_lwmutex_attribute_initialize(compat_attr);
    if (compat_attr.attr_protocol != SYS_SYNC_PRIORITY) return 10;

    return 0;
}

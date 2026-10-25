/* Tests the installed target libraries; no replacement provider in success mode.
   Different/nonzero samples are a smoke check, not an entropy-quality test. */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <random>
#include <exception>
#include <system_error>
#include <thread>
#include <unistd.h>

#ifdef TEST_PROVIDER_ERROR
static unsigned provider_calls;
extern "C" int __wrap_sys_get_random_number(void *, uint64_t)
{
    ++provider_calls;
    return EAGAIN;
}
#endif
#ifdef TEST_ARC4_FAILURE
extern "C" int __wrap_getentropy(void *, size_t)
{
    errno = EIO;
    std::puts("ENTROPY_ARC4_INJECTED_FAILURE");
    std::fflush(stdout);
    return -1;
}
#endif

int main()
{
#ifdef TEST_ARC4_FAILURE
    /* Must terminate through abort; a returned value is always a failure. */
    volatile uint32_t value = arc4random();
    std::printf("ENTROPY_FAIL arc4random returned %u\n", (unsigned)value);
    return 1;
#elif defined(TEST_PROVIDER_ERROR)
    unsigned char buf[4];
    errno = 0;
    if (getentropy(buf, sizeof buf) != -1 || errno != EAGAIN || provider_calls != 1) {
        std::puts("ENTROPY_FAIL provider error propagation");
        return 1;
    }
    try {
        std::random_device rd;
        (void)rd();
    } catch (const std::system_error &) {
        if (provider_calls < 2) {
            std::puts("ENTROPY_FAIL random_device did not use provider");
            return 1;
        }
        std::puts("ENTROPY_ERROR_OK");
        return 0;
    } catch (const std::exception &e) {
        std::printf("ENTROPY_FAIL unexpected exception %s\n", e.what());
        return 1;
    }
    std::puts("ENTROPY_FAIL failed provider accepted");
    return 1;
#else
    unsigned char buf[258], previous[256];
    const size_t lengths[] = {1, 4, 40, 256};
    if (getentropy(nullptr, 0) != 0) {
        std::puts("ENTROPY_FAIL empty");
        return 1;
    }
    for (size_t n : lengths) {
        std::memset(buf, 0x5c, sizeof buf);
        if (getentropy(buf + 1, n) != 0 || buf[0] != 0x5c || buf[n + 1] != 0x5c) {
            std::printf("ENTROPY_FAIL length %u errno %x\n", (unsigned)n, errno);
            return 1;
        }
    }
    std::memcpy(previous, buf + 1, sizeof previous);
    if (getentropy(buf + 1, 256) != 0 || std::memcmp(previous, buf + 1, 256) == 0) {
        std::puts("ENTROPY_FAIL repeated sample");
        return 1;
    }
    errno = 0;
    if (getentropy(buf, 257) != -1 || errno != EIO) {
        std::puts("ENTROPY_FAIL oversize");
        return 1;
    }
    errno = 0;
    if (getentropy(nullptr, 1) != -1 || errno != EFAULT) {
        std::puts("ENTROPY_FAIL null");
        return 1;
    }
    try {
        std::random_device rd;
        unsigned first = rd(), different = 0;
        for (unsigned i = 0; i < 16; ++i) different |= rd() ^ first;
        if (!different) {
            std::puts("ENTROPY_FAIL random_device constant");
            return 1;
        }
    } catch (const std::exception &e) {
        std::printf("ENTROPY_FAIL random_device %s\n", e.what());
        return 1;
    }
    bool worker_ok = false;
    try {
        std::thread worker([&] {
            try {
                std::random_device rd;
                (void)rd();
                worker_ok = true;
            } catch (const std::exception &) {
                // main reports failure after joining; no success on exception.
            }
        });
        worker.join();
    } catch (const std::exception &e) {
        std::printf("ENTROPY_FAIL worker %s\n", e.what());
        return 1;
    }
    if (!worker_ok) {
        std::puts("ENTROPY_FAIL worker random_device");
        return 1;
    }
    uint32_t first_arc4 = arc4random(), different_arc4 = 0;
    for (unsigned i = 0; i < 16; ++i) different_arc4 |= arc4random() ^ first_arc4;
    if (!different_arc4) {
        std::puts("ENTROPY_FAIL arc4random constant");
        return 1;
    }
    std::puts("ENTROPY_OK");
    return 0;
#endif
}

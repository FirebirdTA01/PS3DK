#include <pthread.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

static pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t static_recursive_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_cond_t shared_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t shared_once = PTHREAD_ONCE_INIT;
static pthread_key_t worker_key;

static int failures;
static int once_calls;
static int ready_workers;
static int release_workers;
static int completed_workers;
static int worker_failures;

static void check_true(const char *label, int ok)
{
    if (ok) {
        printf("PASS %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures++;
    }
}

static void check_int(const char *label, int actual, int expected)
{
    if (actual == expected) {
        printf("PASS %s = %d\n", label, actual);
    } else {
        printf("FAIL %s got %d expected %d\n", label, actual, expected);
        failures++;
    }
}

static void check_worker(int ok)
{
    if (!ok) {
        worker_failures++;
    }
}

static void once_body(void)
{
    once_calls++;
}

static void *worker_main(void *arg)
{
    intptr_t worker_id = (intptr_t)arg;
    void *specific_value = (void *)(uintptr_t)(0xCAFE0000u + (unsigned)worker_id);

    check_worker(pthread_once(&shared_once, once_body) == 0);
    check_worker(pthread_setspecific(worker_key, specific_value) == 0);
    check_worker(pthread_getspecific(worker_key) == specific_value);

    check_worker(pthread_mutex_lock(&static_recursive_mutex) == 0);
    check_worker(pthread_mutex_lock(&static_recursive_mutex) == 0);
    check_worker(pthread_mutex_unlock(&static_recursive_mutex) == 0);
    check_worker(pthread_mutex_unlock(&static_recursive_mutex) == 0);

    check_worker(pthread_equal(pthread_self(), pthread_self()) != 0);

    check_worker(pthread_mutex_lock(&shared_mutex) == 0);
    ready_workers++;
    check_worker(pthread_cond_broadcast(&shared_cond) == 0);
    while (!release_workers) {
        check_worker(pthread_cond_wait(&shared_cond, &shared_mutex) == 0);
    }
    completed_workers++;
    check_worker(pthread_cond_signal(&shared_cond) == 0);
    check_worker(pthread_mutex_unlock(&shared_mutex) == 0);

    return (void *)(uintptr_t)(worker_id + 100);
}

int main(void)
{
    pthread_t workers[2];
    pthread_mutex_t attr_recursive_mutex;
    pthread_mutexattr_t mutex_attr;
    int created_workers = 0;

    printf("hello-ppu-pthread: pthread shim sample\n");

    check_int("pthread_key_create", pthread_key_create(&worker_key, NULL), 0);
    check_int("pthread_once first", pthread_once(&shared_once, once_body), 0);
    check_int("pthread_once second", pthread_once(&shared_once, once_body), 0);
    check_int("pthread_once calls", once_calls, 1);

    check_int("pthread_mutexattr_init", pthread_mutexattr_init(&mutex_attr), 0);
    check_int("pthread_mutexattr_settype recursive",
              pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE), 0);
    check_int("pthread_mutex_init recursive",
              pthread_mutex_init(&attr_recursive_mutex, &mutex_attr), 0);
    check_int("pthread_mutex_lock recursive first",
              pthread_mutex_lock(&attr_recursive_mutex), 0);
    check_int("pthread_mutex_lock recursive second",
              pthread_mutex_lock(&attr_recursive_mutex), 0);
    check_int("pthread_mutex_unlock recursive first",
              pthread_mutex_unlock(&attr_recursive_mutex), 0);
    check_int("pthread_mutex_unlock recursive second",
              pthread_mutex_unlock(&attr_recursive_mutex), 0);
    check_int("pthread_mutex_destroy recursive",
              pthread_mutex_destroy(&attr_recursive_mutex), 0);
    check_int("pthread_mutexattr_destroy", pthread_mutexattr_destroy(&mutex_attr), 0);

    check_int("pthread_mutex_lock shared", pthread_mutex_lock(&shared_mutex), 0);
    check_int("pthread_mutex_trylock busy", pthread_mutex_trylock(&shared_mutex), EBUSY);
    check_int("pthread_mutex_unlock shared", pthread_mutex_unlock(&shared_mutex), 0);

    for (intptr_t i = 0; i < 2; i++) {
        int rc = pthread_create(&workers[i], NULL, worker_main, (void *)i);
        check_int(i == 0 ? "pthread_create worker 0" : "pthread_create worker 1", rc, 0);
        if (rc == 0) {
            created_workers++;
        }
    }

    check_int("pthread_mutex_lock wait", pthread_mutex_lock(&shared_mutex), 0);
    while (ready_workers < created_workers) {
        check_int("pthread_cond_wait ready",
                  pthread_cond_wait(&shared_cond, &shared_mutex), 0);
    }
    release_workers = 1;
    check_int("pthread_cond_broadcast", pthread_cond_broadcast(&shared_cond), 0);
    while (completed_workers < created_workers) {
        check_int("pthread_cond_wait complete",
                  pthread_cond_wait(&shared_cond, &shared_mutex), 0);
    }
    check_int("pthread_mutex_unlock wait", pthread_mutex_unlock(&shared_mutex), 0);

    for (intptr_t i = 0; i < created_workers; i++) {
        void *retval = NULL;
        int rc = pthread_join(workers[i], &retval);
        check_int(i == 0 ? "pthread_join worker 0" : "pthread_join worker 1", rc, 0);
        check_true(i == 0 ? "pthread_join retval 0" : "pthread_join retval 1",
                   retval == (void *)(uintptr_t)(i + 100));
    }

    check_int("pthread worker failures", worker_failures, 0);
    check_int("pthread ready workers", ready_workers, created_workers);
    check_int("pthread completed workers", completed_workers, created_workers);
    check_int("pthread_key_delete", pthread_key_delete(worker_key), 0);

    printf("hello-ppu-pthread failures=%d\n", failures);
    if (failures == 0) {
        printf("PTHREAD_OK\n");
        return 0;
    }

    printf("PTHREAD_FAIL\n");
    return 1;
}

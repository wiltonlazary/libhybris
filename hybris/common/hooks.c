/*
 * Copyright (c) 2012 Carsten Munk <carsten.munk@gmail.com>
 * Copyright (c) 2012 Canonical Ltd
 * Copyright (c) 2013 Christophe Chapuis <chris.chapuis@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "config.h"

#include <hybris/common/binding.h>

#include "hooks_shm.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdio_ext.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <inttypes.h>
#include <strings.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/xattr.h>
#include <grp.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <stdarg.h>
#include <wchar.h>
#include <sched.h>
#include <pwd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/signalfd.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <netdb.h>
#include <unistd.h>
#include <syslog.h>
#include <locale.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <sys/prctl.h>

#include <sys/mman.h>
#include <libgen.h>
#include <mntent.h>

#include <hybris/properties/properties.h>
#include <hybris/common/hooks.h>

static locale_t hybris_locale;
static int locale_inited = 0;
static hybris_hook_cb hook_callback = NULL;

static void (*_android_linker_init)(int sdk_version, void* (*get_hooked_symbol)(const char*, const char*)) = NULL;
static void* (*_android_dlopen)(const char *filename, int flags) = NULL;
static void* (*_android_dlsym)(void *handle, const char *symbol) = NULL;
static void* (*_android_dladdr)(void *addr, Dl_info *info) = NULL;
static int (*_android_dlclose)(void *handle) = NULL;
static const char* (*_android_dlerror)(void) = NULL;

/* TODO:
*  - Check if the int arguments at attr_set/get match the ones at Android
*  - Check how to deal with memory leaks (specially with static initializers)
*  - Check for shared rwlock
*/

/* Base address to check for Android specifics */
#define ANDROID_TOP_ADDR_VALUE_MUTEX  0xFFFF
#define ANDROID_TOP_ADDR_VALUE_COND   0xFFFF
#define ANDROID_TOP_ADDR_VALUE_RWLOCK 0xFFFF

#define ANDROID_MUTEX_SHARED_MASK      0x2000
#define ANDROID_COND_SHARED_MASK       0x0001
#define ANDROID_COND_COUNTER_INCREMENT 0x0002
#define ANDROID_COND_COUNTER_MASK      (~ANDROID_COND_SHARED_MASK)
#define ANDROID_RWLOCKATTR_SHARED_MASK 0x0010

#define ANDROID_COND_IS_SHARED(c)  (((c)->value & ANDROID_COND_SHARED_MASK) != 0)

/* For the static initializer types */
#define ANDROID_PTHREAD_MUTEX_INITIALIZER            0
#define ANDROID_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define ANDROID_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000
#define ANDROID_PTHREAD_COND_INITIALIZER             0
#define ANDROID_PTHREAD_RWLOCK_INITIALIZER           0

#define MALI_HIST_DUMP_THREAD_NAME "mali-hist-dump"

/* Debug */
#include "logging.h"
#define LOGD(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

#ifdef DEBUG
#define TRACE_HOOK(message, ...) \
    if (hybris_should_trace(NULL, NULL)) { \
        HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__); \
    }
#else
#define TRACE_HOOK(message, ...)
#endif

/* we have a value p:
 *  - if p <= ANDROID_TOP_ADDR_VALUE_MUTEX then it is an android mutex, not one we processed
 *  - if p > VMALLOC_END, then the pointer is not a result of malloc ==> it is an shm offset
 */

struct _hook {
    const char *name;
    void *func;
};

/* pthread cond struct as done in Android */
typedef struct {
    int volatile value;
} android_cond_t;

/* Helpers */
static int hybris_check_android_shared_mutex(uintptr_t mutex_addr)
{
    /* If not initialized or initialized by Android, it should contain a low
     * address, which is basically just the int values for Android's own
     * pthread_mutex_t */
    if ((mutex_addr <= ANDROID_TOP_ADDR_VALUE_MUTEX) &&
                    (mutex_addr & ANDROID_MUTEX_SHARED_MASK))
        return 1;

    return 0;
}

static int hybris_check_android_shared_cond(uintptr_t cond_addr)
{
    /* If not initialized or initialized by Android, it should contain a low
     * address, which is basically just the int values for Android's own
     * pthread_cond_t */
    if ((cond_addr <= ANDROID_TOP_ADDR_VALUE_COND) &&
                    (cond_addr & ANDROID_COND_SHARED_MASK))
        return 1;

    /* In case android is setting up cond_addr with a negative value,
     * used for error control */
    if (cond_addr > HYBRIS_SHM_MASK_TOP)
        return 1;

    return 0;
}

/* Based on Android's Bionic pthread implementation.
 * This is just needed when we have a shared cond with Android */
static int __android_pthread_cond_pulse(android_cond_t *cond, int counter)
{
    long flags;
    int fret;

    if (cond == NULL)
        return EINVAL;

    flags = (cond->value & ~ANDROID_COND_COUNTER_MASK);
    for (;;) {
        long oldval = cond->value;
        long newval = 0;
        /* In our case all we need to do is make sure the negative value
         * is under our range, which is the last 0xF from SHM_MASK */
        if (oldval < -12)
            newval = ((oldval + ANDROID_COND_COUNTER_INCREMENT) &
                            ANDROID_COND_COUNTER_MASK) | flags;
        else
            newval = ((oldval - ANDROID_COND_COUNTER_INCREMENT) &
                            ANDROID_COND_COUNTER_MASK) | flags;
        if (__sync_bool_compare_and_swap(&cond->value, oldval, newval))
            break;
    }

    int pshared = cond->value & ANDROID_COND_SHARED_MASK;
    fret = syscall(SYS_futex , &cond->value,
                   pshared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE, counter,
                   NULL, NULL, NULL);
    LOGD("futex based pthread_cond_*, value %d, counter %d, ret %d",
                                            cond->value, counter, fret);
    return 0;
}

int android_pthread_cond_broadcast(android_cond_t *cond)
{
    return __android_pthread_cond_pulse(cond, INT_MAX);
}

int android_pthread_cond_signal(android_cond_t *cond)
{
    return __android_pthread_cond_pulse(cond, 1);
}

static void hybris_set_mutex_attr(unsigned int android_value, pthread_mutexattr_t *attr)
{
    /* Init already sets as PTHREAD_MUTEX_NORMAL */
    pthread_mutexattr_init(attr);

    if (android_value & ANDROID_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) {
        pthread_mutexattr_settype(attr, PTHREAD_MUTEX_RECURSIVE);
    } else if (android_value & ANDROID_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
        pthread_mutexattr_settype(attr, PTHREAD_MUTEX_ERRORCHECK);
    }
}

static pthread_mutex_t* hybris_alloc_init_mutex(unsigned int android_mutex)
{
    pthread_mutex_t *realmutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    hybris_set_mutex_attr(android_mutex, &attr);
    pthread_mutex_init(realmutex, &attr);
    return realmutex;
}

static pthread_cond_t* hybris_alloc_init_cond(void)
{
    pthread_cond_t *realcond = malloc(sizeof(pthread_cond_t));
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_cond_init(realcond, &attr);
    return realcond;
}

static pthread_rwlock_t* hybris_alloc_init_rwlock(void)
{
    pthread_rwlock_t *realrwlock = malloc(sizeof(pthread_rwlock_t));
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlock_init(realrwlock, &attr);
    return realrwlock;
}

/*
 * utils, such as malloc, memcpy
 *
 * Useful to handle hacks such as the one applied for Nvidia, and to
 * avoid crashes. Also we need to hook all memory allocation related
 * ones to make sure all are using the same allocator implementation.
 *
 * */

static void *_hybris_hook_malloc(size_t size)
{
    TRACE_HOOK("size %zu", size);

    void *res = malloc(size);

    TRACE_HOOK("res %p", res);

    return res;
}

static size_t _hybris_hook_malloc_usable_size (void *ptr)
{
    TRACE_HOOK("ptr %p", ptr);

    return malloc_usable_size(ptr);
}

static void *_hybris_hook_memcpy(void *dst, const void *src, size_t len)
{
    TRACE_HOOK("dst %p src %p len %zu", dst, src, len);

    if (src == NULL || dst == NULL)
        return dst;

    return memcpy(dst, src, len);
}

static int _hybris_hook_memcmp(const void *s1, const void *s2, size_t n)
{
    TRACE_HOOK("s1 %p '%s' s2 %p '%s' n %zu", s1, (char*) s1, s2, (char*) s2, n);

    return memcmp(s1, s2, n);
}

static size_t _hybris_hook_strlen(const char *s)
{
    TRACE_HOOK("s '%s'", s);

    if (s == NULL)
        return -1;

    return strlen(s);
}

static pid_t _hybris_hook_gettid(void)
{
    TRACE_HOOK("");

    return syscall(__NR_gettid);
}

/*
 * Main pthread functions
 *
 * Custom implementations to workaround difference between Bionic and Glibc.
 * Our own pthread_create helps avoiding direct handling of TLS.
 *
 * */

static int _hybris_hook_pthread_create(pthread_t *thread, const pthread_attr_t *__attr,
                             void *(*start_routine)(void*), void *arg)
{
    pthread_attr_t *realattr = NULL;

    TRACE_HOOK("thread %p attr %p", thread, __attr);

    if (__attr != NULL)
        realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    return pthread_create(thread, realattr, start_routine, arg);
}

static int _hybris_hook_pthread_kill(pthread_t thread, int sig)
{
    TRACE_HOOK("thread %llu sig %d", (unsigned long long) thread, sig);

    if (thread == 0)
        return ESRCH;

    return pthread_kill(thread, sig);
}

static int _hybris_hook_pthread_setspecific(pthread_key_t key, const void *ptr)
{
    TRACE_HOOK("key %d ptr %" PRIdPTR, key, (intptr_t) ptr);

    return pthread_setspecific(key, ptr);
}

static void* _hybris_hook_pthread_getspecific(pthread_key_t key)
{
    TRACE_HOOK("key %d", key);

    // see android_bionic/tests/pthread_test.cpp, test static_pthread_key_used_before_creation
    if(!key) return NULL;

    return pthread_getspecific(key);
}

/*
 * pthread_attr_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_attr_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_attr_init(pthread_attr_t *__attr)
{
    pthread_attr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_attr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_attr_init(realattr);
}

static int _hybris_hook_pthread_attr_destroy(pthread_attr_t *__attr)
{
    int ret;
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    ret = pthread_attr_destroy(realattr);
    /* We need to release the memory allocated at _hybris_hook_pthread_attr_init
     * Possible side effects if destroy is called without our init */
    free(realattr);

    return ret;
}

static int _hybris_hook_pthread_attr_setdetachstate(pthread_attr_t *__attr, int state)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p state %d", __attr, state);

    return pthread_attr_setdetachstate(realattr, state);
}

static int _hybris_hook_pthread_attr_getdetachstate(pthread_attr_t const *__attr, int *state)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p state %p", __attr, state);

    return pthread_attr_getdetachstate(realattr, state);
}

static int _hybris_hook_pthread_attr_setschedpolicy(pthread_attr_t *__attr, int policy)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p policy %d", __attr, policy);

    return pthread_attr_setschedpolicy(realattr, policy);
}

static int _hybris_hook_pthread_attr_getschedpolicy(pthread_attr_t const *__attr, int *policy)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p policy %p", __attr, policy);

    return pthread_attr_getschedpolicy(realattr, policy);
}

static int _hybris_hook_pthread_attr_setschedparam(pthread_attr_t *__attr, struct sched_param const *param)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p param %p", __attr, param);

    return pthread_attr_setschedparam(realattr, param);
}

static int _hybris_hook_pthread_attr_getschedparam(pthread_attr_t const *__attr, struct sched_param *param)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p param %p", __attr, param);

    return pthread_attr_getschedparam(realattr, param);
}

static int _hybris_hook_pthread_attr_setstacksize(pthread_attr_t *__attr, size_t stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack size %zu", __attr, stack_size);

    return pthread_attr_setstacksize(realattr, stack_size);
}

static int _hybris_hook_pthread_attr_getstacksize(pthread_attr_t const *__attr, size_t *stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack size %p", __attr, stack_size);

    return pthread_attr_getstacksize(realattr, stack_size);
}

static int _hybris_hook_pthread_attr_setstackaddr(pthread_attr_t *__attr, void *stack_addr)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack addr %p", __attr, stack_addr);

    return pthread_attr_setstackaddr(realattr, stack_addr);
}

static int _hybris_hook_pthread_attr_getstackaddr(pthread_attr_t const *__attr, void **stack_addr)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack addr %p", __attr, stack_addr);

    return pthread_attr_getstackaddr(realattr, stack_addr);
}

static int _hybris_hook_pthread_attr_setstack(pthread_attr_t *__attr, void *stack_base, size_t stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack base %p stack size %zu", __attr,
               stack_base, stack_size);

    return pthread_attr_setstack(realattr, stack_base, stack_size);
}

static int _hybris_hook_pthread_attr_getstack(pthread_attr_t const *__attr, void **stack_base, size_t *stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack base %p stack size %p", __attr,
               stack_base, stack_size);

    return pthread_attr_getstack(realattr, stack_base, stack_size);
}

static int _hybris_hook_pthread_attr_setguardsize(pthread_attr_t *__attr, size_t guard_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p guard size %zu", __attr, guard_size);

    return pthread_attr_setguardsize(realattr, guard_size);
}

static int _hybris_hook_pthread_attr_getguardsize(pthread_attr_t const *__attr, size_t *guard_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p guard size %p", __attr, guard_size);

    return pthread_attr_getguardsize(realattr, guard_size);
}

static int _hybris_hook_pthread_attr_setscope(pthread_attr_t *__attr, int scope)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p scope %d", __attr, scope);

    return pthread_attr_setscope(realattr, scope);
}

static int _hybris_hook_pthread_attr_getscope(pthread_attr_t const *__attr)
{
    int scope;
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    /* Android doesn't have the scope attribute because it always
     * returns PTHREAD_SCOPE_SYSTEM */
    pthread_attr_getscope(realattr, &scope);

    return scope;
}

static int _hybris_hook_pthread_getattr_np(pthread_t thid, pthread_attr_t *__attr)
{
    pthread_attr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_attr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_getattr_np(thid, realattr);
}

/*
 * pthread_mutex* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_mutex_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_mutex_init(pthread_mutex_t *__mutex,
                          __const pthread_mutexattr_t *__mutexattr)
{
    pthread_mutex_t *realmutex = NULL;

    TRACE_HOOK("mutex %p attr %p", __mutex, __mutexattr);

    int pshared = 0;
    if (__mutexattr)
        pthread_mutexattr_getpshared(__mutexattr, &pshared);

    if (!pshared) {
        /* non shared, standard mutex: use malloc */
        realmutex = malloc(sizeof(pthread_mutex_t));

        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }
    else {
        /* process-shared mutex: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_mutex_t));

        *((hybris_shm_pointer_t *)__mutex) = handle;

        if (handle)
            realmutex = (pthread_mutex_t *)hybris_get_shmpointer(handle);
    }

    return pthread_mutex_init(realmutex, __mutexattr);
}

static int _hybris_hook_pthread_mutex_destroy(pthread_mutex_t *__mutex)
{
    int ret;

    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex)
        return EINVAL;

    pthread_mutex_t *realmutex = (pthread_mutex_t *) *(uintptr_t *) __mutex;

    if (!realmutex)
        return EINVAL;

    if (!hybris_is_pointer_in_shm((void*)realmutex)) {
        ret = pthread_mutex_destroy(realmutex);
        free(realmutex);
    }
    else {
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)realmutex);
        ret = pthread_mutex_destroy(realmutex);
    }

    *((uintptr_t *)__mutex) = 0;

    return ret;
}

static int _hybris_hook_pthread_mutex_lock(pthread_mutex_t *__mutex)
{
    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex) {
        LOGD("Null mutex lock, not locking.");
        return 0;
    }

    uintptr_t value = (*(uintptr_t *) __mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not locking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        TRACE("value %p <= ANDROID_TOP_ADDR_VALUE_MUTEX 0x%x",
              (void*) value, ANDROID_TOP_ADDR_VALUE_MUTEX);
        realmutex = hybris_alloc_init_mutex(value);
        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }

    return pthread_mutex_lock(realmutex);
}

static int _hybris_hook_pthread_mutex_trylock(pthread_mutex_t *__mutex)
{
    uintptr_t value = (*(uintptr_t *) __mutex);

    TRACE_HOOK("mutex %p", __mutex);

    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not try locking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(value);
        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }

    return pthread_mutex_trylock(realmutex);
}

static int _hybris_hook_pthread_mutex_unlock(pthread_mutex_t *__mutex)
{
    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex) {
        LOGD("Null mutex lock, not unlocking.");
        return 0;
    }

    uintptr_t value = (*(uintptr_t *) __mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not unlocking.");
        return 0;
    }

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        LOGD("Trying to unlock a lock that's not locked/initialized"
               " by Hybris, not unlocking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    return pthread_mutex_unlock(realmutex);
}

static int _hybris_hook_pthread_mutex_lock_timeout_np(pthread_mutex_t *__mutex, unsigned __msecs)
{
    struct timespec tv;
    pthread_mutex_t *realmutex;
    uintptr_t value = (*(uintptr_t *) __mutex);

    TRACE_HOOK("mutex %p msecs %u", __mutex, __msecs);

    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not lock timeout np.");
        return 0;
    }

    realmutex = (pthread_mutex_t *) value;

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(value);
        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }

    clock_gettime(CLOCK_REALTIME, &tv);
    tv.tv_sec += __msecs/1000;
    tv.tv_nsec += (__msecs % 1000) * 1000000;
    if (tv.tv_nsec >= 1000000000) {
      tv.tv_sec++;
      tv.tv_nsec -= 1000000000;
    }

    return pthread_mutex_timedlock(realmutex, &tv);
}

static int _hybris_hook_pthread_mutex_timedlock(pthread_mutex_t *__mutex,
                                      const struct timespec *__abs_timeout)
{
    TRACE_HOOK("mutex %p abs timeout %p", __mutex, __abs_timeout);

    if (!__mutex) {
        LOGD("Null mutex lock, not unlocking.");
        return 0;
    }

    uintptr_t value = (*(uintptr_t *) __mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not lock timeout np.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(value);
        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }

    return pthread_mutex_timedlock(realmutex, __abs_timeout);
}

static int _hybris_hook_pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr,
                                           int pshared)
{
    TRACE_HOOK("attr %p pshared %d", __attr, pshared);

    return pthread_mutexattr_setpshared(__attr, pshared);
}

/*
 * pthread_cond* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_cond_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_cond_init(pthread_cond_t *cond,
                                const pthread_condattr_t *attr)
{
    pthread_cond_t *realcond = NULL;

    TRACE_HOOK("cond %p attr %p", cond, attr);

    int pshared = 0;

    if (attr)
        pthread_condattr_getpshared(attr, &pshared);

    if (!pshared) {
        /* non shared, standard cond: use malloc */
        realcond = malloc(sizeof(pthread_cond_t));

        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }
    else {
        /* process-shared condition: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_cond_t));

        *((uintptr_t *)cond) = (uintptr_t) handle;

        if (handle)
            realcond = (pthread_cond_t *)hybris_get_shmpointer(handle);
    }

    return pthread_cond_init(realcond, attr);
}

static int _hybris_hook_pthread_cond_destroy(pthread_cond_t *cond)
{
    int ret;
    pthread_cond_t *realcond = (pthread_cond_t *) *(uintptr_t *) cond;

    TRACE_HOOK("cond %p", cond);

    if (!realcond) {
      return EINVAL;
    }

    if (!hybris_is_pointer_in_shm((void*)realcond)) {
        ret = pthread_cond_destroy(realcond);
        free(realcond);
    }
    else {
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)realcond);
        ret = pthread_cond_destroy(realcond);
    }

    *((uintptr_t *)cond) = 0;

    return ret;
}

static int _hybris_hook_pthread_cond_broadcast(pthread_cond_t *cond)
{
    uintptr_t value = (*(uintptr_t *) cond);

    TRACE_HOOK("cond %p", cond);

    if (hybris_check_android_shared_cond(value)) {
        LOGD("Shared condition with Android, broadcasting with futex.");
        return android_pthread_cond_broadcast((android_cond_t *) cond);
    }

    pthread_cond_t *realcond = (pthread_cond_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_alloc_init_cond();
        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }

    return pthread_cond_broadcast(realcond);
}

static int _hybris_hook_pthread_cond_signal(pthread_cond_t *cond)
{
    uintptr_t value = (*(uintptr_t *) cond);

    TRACE_HOOK("cond %p", cond);

    if (hybris_check_android_shared_cond(value)) {
        LOGD("Shared condition with Android, broadcasting with futex.");
        return android_pthread_cond_signal((android_cond_t *) cond);
    }

    pthread_cond_t *realcond = (pthread_cond_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_alloc_init_cond();
        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }

    return pthread_cond_signal(realcond);
}

static int _hybris_hook_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = (*(uintptr_t *) cond);
    uintptr_t mvalue = (*(uintptr_t *) mutex);

    TRACE_HOOK("cond %p mutex %p", cond, mutex);

    if (hybris_check_android_shared_cond(cvalue) ||
        hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if (hybris_is_pointer_in_shm((void*)cvalue))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_alloc_init_cond();
        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(mvalue);
        *((uintptr_t *) mutex) = (uintptr_t) realmutex;
    }

    return pthread_cond_wait(realcond, realmutex);
}

static int _hybris_hook_pthread_cond_timedwait(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *abstime)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = (*(uintptr_t *) cond);
    uintptr_t mvalue = (*(uintptr_t *) mutex);

    TRACE_HOOK("cond %p mutex %p abstime %p", cond, mutex, abstime);

    if (hybris_check_android_shared_cond(cvalue) ||
         hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if (hybris_is_pointer_in_shm((void*)cvalue))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_alloc_init_cond();
        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(mvalue);
        *((uintptr_t *) mutex) = (uintptr_t) realmutex;
    }

    return pthread_cond_timedwait(realcond, realmutex, abstime);
}

static int _hybris_hook_pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *reltime)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = (*(uintptr_t *) cond);
    uintptr_t mvalue = (*(uintptr_t *) mutex);

    TRACE_HOOK("cond %p mutex %p reltime %p", cond, mutex, reltime);

    if (hybris_check_android_shared_cond(cvalue) ||
         hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if( hybris_is_pointer_in_shm((void*)cvalue) )
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_alloc_init_cond();
        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_alloc_init_mutex(mvalue);
        *((uintptr_t *) mutex) = (uintptr_t) realmutex;
    }

    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    tv.tv_sec += reltime->tv_sec;
    tv.tv_nsec += reltime->tv_nsec;
    if (tv.tv_nsec >= 1000000000) {
      tv.tv_sec++;
      tv.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(realcond, realmutex, &tv);
}

int _hybris_hook_pthread_setname_np(pthread_t thread, const char *name)
{
    TRACE_HOOK("thread %llu name %s", (unsigned long long) thread, name);

#ifdef MALI_QUIRKS
    if (strcmp(name, MALI_HIST_DUMP_THREAD_NAME) == 0) {
        HYBRIS_DEBUG_LOG(HOOKS, "%s: Found mali-hist-dump thread, killing it ...",
                         __FUNCTION__);

        if (thread != pthread_self()) {
            HYBRIS_DEBUG_LOG(HOOKS, "%s: -> Failed, as calling thread is not mali-hist-dump itself",
                             __FUNCTION__);
            return 0;
        }

        pthread_exit((void*) thread);

        return 0;
    }
#endif

    return pthread_setname_np(thread, name);
}

/*
 * pthread_rwlockattr_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_rwlockattr_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_rwlockattr_init(pthread_rwlockattr_t *__attr)
{
    pthread_rwlockattr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_rwlockattr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_rwlockattr_init(realattr);
}

static int _hybris_hook_pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr)
{
    int ret;
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    ret = pthread_rwlockattr_destroy(realattr);
    free(realattr);

    return ret;
}

static int _hybris_hook_pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr,
                                            int pshared)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p pshared %d", __attr, pshared);

    return pthread_rwlockattr_setpshared(realattr, pshared);
}

static int _hybris_hook_pthread_rwlockattr_getpshared(pthread_rwlockattr_t *__attr,
                                            int *pshared)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p pshared %p", __attr, pshared);

    return pthread_rwlockattr_getpshared(realattr, pshared);
}

int _hybris_hook_pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *attr, int pref)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) attr;

    TRACE_HOOK("attr %p pref %i", attr, pref);

    return pthread_rwlockattr_setkind_np(realattr, pref);
}

int _hybris_hook_pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *attr, int *pref)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) attr;

    TRACE_HOOK("attr %p pref %p", attr, pref);

    return pthread_rwlockattr_getkind_np(realattr, pref);
}

/*
 * pthread_rwlock_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_rwlock_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_rwlock_init(pthread_rwlock_t *__rwlock,
                                  __const pthread_rwlockattr_t *__attr)
{
    pthread_rwlock_t *realrwlock = NULL;
    pthread_rwlockattr_t *realattr = NULL;
    int pshared = 0;

    TRACE_HOOK("rwlock %p attr %p", __rwlock, __attr);

    if (__attr != NULL)
        realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    if (realattr)
        pthread_rwlockattr_getpshared(realattr, &pshared);

    if (!pshared) {
        /* non shared, standard rwlock: use malloc */
        realrwlock = malloc(sizeof(pthread_rwlock_t));

        *((uintptr_t *) __rwlock) = (uintptr_t) realrwlock;
    }
    else {
        /* process-shared condition: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_rwlock_t));

        *((uintptr_t *)__rwlock) = (uintptr_t) handle;

        if (handle)
            realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer(handle);
    }

    return pthread_rwlock_init(realrwlock, realattr);
}

static int _hybris_hook_pthread_rwlock_destroy(pthread_rwlock_t *__rwlock)
{
    int ret;
    pthread_rwlock_t *realrwlock = (pthread_rwlock_t *) *(uintptr_t *) __rwlock;

    TRACE_HOOK("rwlock %p", __rwlock);

    if (!hybris_is_pointer_in_shm((void*)realrwlock)) {
        ret = pthread_rwlock_destroy(realrwlock);
        free(realrwlock);
    }
    else {
        ret = pthread_rwlock_destroy(realrwlock);
        realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)realrwlock);
    }

    return ret;
}

static pthread_rwlock_t* hybris_set_realrwlock(pthread_rwlock_t *rwlock)
{
    uintptr_t value = (*(uintptr_t *) rwlock);
    pthread_rwlock_t *realrwlock = (pthread_rwlock_t *) value;

    if (hybris_is_pointer_in_shm((void*)value))
        realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if ((uintptr_t)realrwlock <= ANDROID_TOP_ADDR_VALUE_RWLOCK) {
        realrwlock = hybris_alloc_init_rwlock();
        *((uintptr_t *)rwlock) = (uintptr_t) realrwlock;
    }
    return realrwlock;
}

static int _hybris_hook_pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_rdlock(realrwlock);
}

static int _hybris_hook_pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_tryrdlock(realrwlock);
}

static int _hybris_hook_pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout)
{
    TRACE_HOOK("rwlock %p abs timeout %p", __rwlock, abs_timeout);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_timedrdlock(realrwlock, abs_timeout);
}

static int _hybris_hook_pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_wrlock(realrwlock);
}

static int _hybris_hook_pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_trywrlock(realrwlock);
}

static int _hybris_hook_pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout)
{
    TRACE_HOOK("rwlock %p abs timeout %p", __rwlock, abs_timeout);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_timedwrlock(realrwlock, abs_timeout);
}

static int _hybris_hook_pthread_rwlock_unlock(pthread_rwlock_t *__rwlock)
{
    uintptr_t value = (*(uintptr_t *) __rwlock);

    TRACE_HOOK("rwlock %p", __rwlock);

    if (value <= ANDROID_TOP_ADDR_VALUE_RWLOCK) {
        LOGD("Trying to unlock a rwlock that's not locked/initialized"
               " by Hybris, not unlocking.");
        return 0;
    }

    pthread_rwlock_t *realrwlock = (pthread_rwlock_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    return pthread_rwlock_unlock(realrwlock);
}

#define min(X,Y) (((X) < (Y)) ? (X) : (Y))

static pid_t _hybris_hook_pthread_gettid(pthread_t t)
{
    TRACE_HOOK("thread %lu", (unsigned long) t);

    // glibc doesn't offer us a way to retrieve the thread id for a
    // specific thread. However pthread_t is defined as unsigned
    // long int and is the thread id so we can just copy it over
    // into a pid_t.
    pid_t tid;
    memcpy(&tid, &t, min(sizeof(tid), sizeof(t)));
    return tid;
}

static int _hybris_hook_set_errno(int oi_errno)
{
    TRACE_HOOK("errno %d", oi_errno);

    errno = oi_errno;

    return -1;
}

/*
 * __isthreaded is used in bionic's stdio.h to choose between a fast internal implementation
 * and a more classic stdio function call.
 * For example:
 * #define  __sfeof(p)  (((p)->_flags & __SEOF) != 0)
 * #define  feof(p)     (!__isthreaded ? __sfeof(p) : (feof)(p))
 *
 * We see here that if __isthreaded is false, then it will use directly the bionic's FILE structure
 * instead of calling one of the hooked methods.
 * Therefore we need to set __isthreaded to true, even if we are not in a multi-threaded context.
 */
static int ___hybris_hook_isthreaded = 1;

/* "struct __sbuf" from bionic/libc/include/stdio.h */
#if defined(__LP64__)
struct bionic_sbuf {
    unsigned char* _base;
    size_t _size;
};
#else
struct bionic_sbuf {
    unsigned char *_base;
    int _size;
};
#endif

typedef off_t bionic_fpos_t;

/* "struct __sFILE" from bionic/libc/include/stdio.h */
struct __attribute__((packed)) bionic_file {
    unsigned char *_p;      /* current position in (some) buffer */
    int _r;                 /* read space left for getc() */
    int _w;                 /* write space left for putc() */
#if defined(__LP64__)
    int _flags;             /* flags, below; this FILE is free if 0 */
    int _file;              /* fileno, if Unix descriptor, else -1 */
#else
    short _flags;           /* flags, below; this FILE is free if 0 */
    short _file;            /* fileno, if Unix descriptor, else -1 */
#endif
    struct bionic_sbuf _bf; /* the buffer (at least 1 byte, if !NULL) */
    int _lbfsize;           /* 0 or -_bf._size, for inline putc */

    /* operations */
    void *_cookie;          /* cookie passed to io functions */
    int (*_close)(void *);
    int (*_read)(void *, char *, int);
    bionic_fpos_t (*_seek)(void *, bionic_fpos_t, int);
    int (*_write)(void *, const char *, int);

    /* extension data, to avoid further ABI breakage */
    struct bionic_sbuf _ext;
    /* data for long sequences of ungetc() */
    unsigned char *_up;     /* saved _p when _p is doing ungetc data */
    int _ur;                /* saved _r when _r is counting ungetc data */

    /* tricks to meet minimum requirements even when malloc() fails */
    unsigned char _ubuf[3]; /* guarantee an ungetc() buffer */
    unsigned char _nbuf[1]; /* guarantee a getc() buffer */

    /* separate buffer for fgetln() when line crosses buffer boundary */
    struct bionic_sbuf _lb; /* buffer for fgetln() */

    /* Unix stdio files get aligned to block boundaries on fseek() */
    int _blksize;           /* stat.st_blksize (may be != _bf._size) */
    bionic_fpos_t _offset;         /* current lseek offset */
};

/*
 * redirection for bionic's __sF, which is defined as:
 *   FILE __sF[3];
 *   #define stdin  &__sF[0];
 *   #define stdout &__sF[1];
 *   #define stderr &__sF[2];
 *   So the goal here is to catch the call to file methods where the FILE* pointer
 *   is either stdin, stdout or stderr, and translate that pointer to a valid glibc
 *   pointer.
 *   Currently, only fputs is managed.
 */
static char _hybris_hook_sF[3 * sizeof(struct bionic_file)] = {0};
static FILE *_get_actual_fp(FILE *fp)
{
    char *c_fp = (char*)fp;
    if (c_fp == &_hybris_hook_sF[0])
        return stdin;
    else if (c_fp == &_hybris_hook_sF[sizeof(struct bionic_file)])
        return stdout;
    else if (c_fp == &_hybris_hook_sF[sizeof(struct bionic_file) * 2])
        return stderr;

    return fp;
}

static void _hybris_hook_clearerr(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    clearerr(_get_actual_fp(fp));
}

static int _hybris_hook_fclose(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fclose(_get_actual_fp(fp));
}

static int _hybris_hook_feof(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return feof(_get_actual_fp(fp));
}

static int _hybris_hook_ferror(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ferror(_get_actual_fp(fp));
}

static int _hybris_hook_fflush(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fflush(_get_actual_fp(fp));
}

static int _hybris_hook_fgetc(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fgetc(_get_actual_fp(fp));
}

static int _hybris_hook_fgetpos(FILE *fp, bionic_fpos_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos_t my_fpos;
    int ret = fgetpos(_get_actual_fp(fp), &my_fpos);

    *pos = my_fpos.__pos;

    return ret;
}

static char* _hybris_hook_fgets(char *s, int n, FILE *fp)
{
    TRACE_HOOK("s %s n %d fp %p", s, n, fp);

    return fgets(s, n, _get_actual_fp(fp));
}

FP_ATTRIB static int _hybris_hook_fprintf(FILE *fp, const char *fmt, ...)
{
    int ret = 0;

    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    va_list args;
    va_start(args,fmt);
    ret = vfprintf(_get_actual_fp(fp), fmt, args);
    va_end(args);

    return ret;
}

static int _hybris_hook_fputc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return fputc(c, _get_actual_fp(fp));
}

static int _hybris_hook_fputs(const char *s, FILE *fp)
{
    TRACE_HOOK("s '%s' fp %p", s, fp);

    return fputs(s, _get_actual_fp(fp));
}

static size_t _hybris_hook_fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fread(ptr, size, nmemb, _get_actual_fp(fp));
}

static FILE* _hybris_hook_freopen(const char *filename, const char *mode, FILE *fp)
{
    TRACE_HOOK("filename '%s' mode '%s' fp %p", filename, mode, fp);

    return freopen(filename, mode, _get_actual_fp(fp));
}

FP_ATTRIB static int _hybris_hook_fscanf(FILE *fp, const char *fmt, ...)
{
    int ret = 0;

    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    va_list args;
    va_start(args,fmt);
    ret = vfscanf(_get_actual_fp(fp), fmt, args);
    va_end(args);

    return ret;
}

static int _hybris_hook_fseek(FILE *fp, long offset, int whence)
{
    TRACE_HOOK("fp %p offset %jd whence %d", fp, offset, whence);

    return fseek(_get_actual_fp(fp), offset, whence);
}

static int _hybris_hook_fseeko(FILE *fp, off_t offset, int whence)
{
    TRACE_HOOK("fp %p offset %jd whence %d", fp, offset, whence);

    return fseeko(_get_actual_fp(fp), offset, whence);
}

static int _hybris_hook_fsetpos(FILE *fp, const bionic_fpos_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos_t my_fpos;
    my_fpos.__pos = *pos;
    memset(&my_fpos.__state, 0, sizeof(mbstate_t));
    mbsinit(&my_fpos.__state);

    return fsetpos(_get_actual_fp(fp), &my_fpos);
}

static long _hybris_hook_ftell(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftell(_get_actual_fp(fp));
}

static off_t _hybris_hook_ftello(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftello(_get_actual_fp(fp));
}

static size_t _hybris_hook_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fwrite(ptr, size, nmemb, _get_actual_fp(fp));
}

static int _hybris_hook_getc(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getc(_get_actual_fp(fp));
}

static ssize_t _hybris_hook_getdelim(char ** lineptr, size_t *n, int delimiter, FILE * fp)
{
    TRACE_HOOK("lineptr %p n %p delimiter %d fp %p", lineptr, n, delimiter, fp);

    return getdelim(lineptr, n, delimiter, _get_actual_fp(fp));
}

static ssize_t _hybris_hook_getline(char **lineptr, size_t *n, FILE *fp)
{
    TRACE_HOOK("lineptr %p n %p fp %p", lineptr, n, fp);

    return getline(lineptr, n, _get_actual_fp(fp));
}

static int _hybris_hook_putc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return putc(c, _get_actual_fp(fp));
}

static void _hybris_hook_rewind(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    rewind(_get_actual_fp(fp));
}

static void _hybris_hook_setbuf(FILE *fp, char *buf)
{
    TRACE_HOOK("fp %p buf '%s'", fp, buf);

    setbuf(_get_actual_fp(fp), buf);
}

static int _hybris_hook_setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    TRACE_HOOK("fp %p buf '%s' mode %d size %zu", fp, buf, mode, size);

    return setvbuf(_get_actual_fp(fp), buf, mode, size);
}

static int _hybris_hook_ungetc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return ungetc(c, _get_actual_fp(fp));
}

static int _hybris_hook_vfprintf(FILE *fp, const char *fmt, va_list arg)
{
    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    return vfprintf(_get_actual_fp(fp), fmt, arg);
}

static int _hybris_hook_vfscanf(FILE *fp, const char *fmt, va_list arg)
{
    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    return vfscanf(_get_actual_fp(fp), fmt, arg);
}

static int _hybris_hook_fileno(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fileno(_get_actual_fp(fp));
}

static int _hybris_hook_pclose(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return pclose(_get_actual_fp(fp));
}

static void _hybris_hook_flockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return flockfile(_get_actual_fp(fp));
}

static int _hybris_hook_ftrylockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftrylockfile(_get_actual_fp(fp));
}

static void _hybris_hook_funlockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return funlockfile(_get_actual_fp(fp));
}

static int _hybris_hook_getc_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getc_unlocked(_get_actual_fp(fp));
}

static int _hybris_hook_putc_unlocked(int c, FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return putc_unlocked(c, _get_actual_fp(fp));
}

/* exists only on the BSD platform
static char* _hybris_hook_fgetln(FILE *fp, size_t *len)
{
    return fgetln(_get_actual_fp(fp), len);
}
*/

static int _hybris_hook_fpurge(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    __fpurge(_get_actual_fp(fp));

    return 0;
}

static int _hybris_hook_getw(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getw(_get_actual_fp(fp));
}

static int _hybris_hook_putw(int w, FILE *fp)
{
    TRACE_HOOK("w %d fp %p", w, fp);

    return putw(w, _get_actual_fp(fp));
}

static void _hybris_hook_setbuffer(FILE *fp, char *buf, int size)
{
    TRACE_HOOK("fp %p buf '%s' size %d", fp, buf, size);

    setbuffer(_get_actual_fp(fp), buf, size);
}

static int _hybris_hook_setlinebuf(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    setlinebuf(_get_actual_fp(fp));

    return 0;
}

/* "struct dirent" from bionic/libc/include/dirent.h */
struct bionic_dirent {
    uint64_t         d_ino;
    int64_t          d_off;
    unsigned short   d_reclen;
    unsigned char    d_type;
    char             d_name[256];
};

static struct bionic_dirent *_hybris_hook_readdir(DIR *dirp)
{
    /**
     * readdir(3) manpage says:
     *  The data returned by readdir() may be overwritten by subsequent calls
     *  to readdir() for the same directory stream.
     *
     * XXX: At the moment, for us, the data will be overwritten even by
     * subsequent calls to /different/ directory streams. Eventually fix that
     * (e.g. by storing per-DIR * bionic_dirent structs, and removing them on
     * closedir, requires hooking of all funcs returning/taking DIR *) and
     * handling the additional data attachment there)
     **/

    static struct bionic_dirent result;

    TRACE_HOOK("dirp %p", dirp);

    struct dirent *real_result = readdir(dirp);
    if (!real_result) {
        return NULL;
    }

    result.d_ino = real_result->d_ino;
    result.d_off = real_result->d_off;
    result.d_reclen = real_result->d_reclen;
    result.d_type = real_result->d_type;
    memcpy(result.d_name, real_result->d_name, sizeof(result.d_name));

    // Make sure the string is zero-terminated, even if cut off (which
    // shouldn't happen, as both bionic and glibc have d_name defined
    // as fixed array of 256 chars)
    result.d_name[sizeof(result.d_name)-1] = '\0';
    return &result;
}

static int _hybris_hook_readdir_r(DIR *dir, struct bionic_dirent *entry,
        struct bionic_dirent **result)
{
    struct dirent entry_r;
    struct dirent *result_r;

    TRACE_HOOK("dir %p entry %p result %p", dir, entry, result);

    int res = readdir_r(dir, &entry_r, &result_r);

    if (res == 0) {
        if (result_r != NULL) {
            *result = entry;

            entry->d_ino = entry_r.d_ino;
            entry->d_off = entry_r.d_off;
            entry->d_reclen = entry_r.d_reclen;
            entry->d_type = entry_r.d_type;
            memcpy(entry->d_name, entry_r.d_name, sizeof(entry->d_name));

            // Make sure the string is zero-terminated, even if cut off (which
            // shouldn't happen, as both bionic and glibc have d_name defined
            // as fixed array of 256 chars)
            entry->d_name[sizeof(entry->d_name) - 1] = '\0';
        } else {
            *result = NULL;
        }
    }

    return res;
}

static int _hybris_hook_alphasort(struct bionic_dirent **a,
                                  struct bionic_dirent **b)
{
    return strcoll((*a)->d_name, (*b)->d_name);
}

static int my_versionsort(struct bionic_dirent **a,
                          struct bionic_dirent **b)
{
    return strverscmp((*a)->d_name, (*b)->d_name);
}

static int my_scandirat(int fd, const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, dir, &namelist_r, NULL, NULL);

    if (res != 0 && namelist_r != NULL) {

        result = malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                        free(result[i]);
                    free(result);
                    return -1;
            }
            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent *), compar);

        *namelist = result;
    }

    return res;
}

static int my_scandir(const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    return my_scandirat(AT_FDCWD, dir, namelist, filter, compar);
}

static int _hybris_hook_versionsort(struct bionic_dirent **a,
                          struct bionic_dirent **b)
{
    return strverscmp((*a)->d_name, (*b)->d_name);
}

static int _hybris_hook_scandirat(int parent_fd, const char* dir_name,
                struct bionic_dirent ***namelist,
		int (*filter)(const struct bionic_dirent *),
		int (*compar)(const struct bionic_dirent **, const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    static struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    TRACE_HOOK("parent_fd %d, dir_name %s, namelist %p, filter %p, compar %p",
               parent_fd, dir_name, namelist, filter, compar);

    int res = scandirat(parent_fd, dir_name, &namelist_r, NULL, NULL);

    if (res != 0 && namelist_r != NULL) {

        result = malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                        free(result[i]);
                    free(result);
                    return -1;
            }
            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        if (nItems && compar != NULL)
            qsort(result, nItems, sizeof(struct bionic_dirent *), (__compar_fn_t) compar);

        *namelist = result;
    }

    return res;
}

static int _hybris_hook_scandir(const char* dir_path, struct bionic_dirent ***namelist,
		int (*filter)(const struct bionic_dirent *),
		int (*compar)(const struct bionic_dirent **, const struct bionic_dirent **))
{
    return _hybris_hook_scandirat(AT_FDCWD, dir_path, namelist, filter, compar);
}

static inline void swap(void **a, void **b)
{
    void *tmp = *a;
    *a = *b;
    *b = tmp;
}

static int _hybris_hook_getaddrinfo(const char *hostname, const char *servname,
    const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo *fixed_hints = NULL;

    TRACE_HOOK("hostname '%s' servname '%s' hints %p res %p",
               hostname, servname, hints, res);

    if (hints) {
        fixed_hints = (struct addrinfo*) malloc(sizeof(struct addrinfo));
        memcpy(fixed_hints, hints, sizeof(struct addrinfo));
        // fix bionic -> glibc missmatch
        swap((void**)&(fixed_hints->ai_canonname), (void**)&(fixed_hints->ai_addr));
    }

    int result = getaddrinfo(hostname, servname, fixed_hints, res);

    if (fixed_hints)
        free(fixed_hints);

    // fix bionic <- glibc missmatch
    struct addrinfo *it = *res;
    while (it) {
        swap((void**) &(it->ai_canonname), (void**) &(it->ai_addr));
        it = it->ai_next;
    }

    return result;
}

static void _hybris_hook_freeaddrinfo(struct addrinfo *__ai)
{
    TRACE_HOOK("ai %p", __ai);

    if (__ai == NULL)
        return;

    struct addrinfo *it = __ai;
    while (it) {
        swap((void**) &(it->ai_canonname), (void**) &(it->ai_addr));
        it = it->ai_next;
    }

    freeaddrinfo(__ai);
}

extern long _hybris_map_sysconf(int name);

long _hybris_hook_sysconf(int name)
{
    TRACE_HOOK("name %d", name);

    return _hybris_map_sysconf(name);
}

FP_ATTRIB static double _hybris_hook_strtod(const char *nptr, char **endptr)
{
    TRACE_HOOK("nptr '%s' endptr %p", nptr, endptr);

    if (locale_inited == 0) {
            hybris_locale = newlocale(LC_ALL_MASK, "C", 0);
            locale_inited = 1;
    }

    return strtod_l(nptr, endptr, hybris_locale);
}

static long int _hybris_hook_strtol(const char* str, char** endptr, int base)
{
    TRACE_HOOK("str '%s' endptr %p base %i", str, endptr, base);

    return strtol(str, endptr, base);
}

static int ___hybris_hook_system_property_read(const void *pi, char *name, char *value)
{
    TRACE_HOOK("pi %p name '%s' value '%s'", pi, name, value);

    return property_get(name, value, NULL);
}

static int ___hybris_hook_system_property_foreach(void (*propfn)(const void *pi, void *cookie), void *cookie)
{
    TRACE_HOOK("propfn %p cookie %p", propfn, cookie);

    return 0;
}

static const void *___hybris_hook_system_property_find(const char *name)
{
    TRACE_HOOK("name '%s'", name);

    return NULL;
}

static unsigned int ___hybris_hook_system_property_serial(const void *pi)
{
    TRACE_HOOK("pi %p", pi);

    return 0;
}

static int ___hybris_hook_system_property_wait(const void *pi)
{
    TRACE_HOOK("pi %p", pi);

    return 0;
}

static int ___hybris_hook_system_property_update(void *pi, const char *value, unsigned int len)
{
    TRACE_HOOK("pi %p value '%s' len %d", pi, value, len);

    return 0;
}

static int ___hybris_hook_system_property_add(const char *name, unsigned int namelen, const char *value, unsigned int valuelen)
{
    TRACE_HOOK("name '%s' namelen %d value '%s' valuelen %d",
               name, namelen, value, valuelen);
    return 0;
}

static unsigned int ___hybris_hook_system_property_wait_any(unsigned int serial)
{
    TRACE_HOOK("serial %d", serial);

    return 0;
}

static const void *___hybris_hook_system_property_find_nth(unsigned n)
{
    TRACE_HOOK("n %d", n);

    return NULL;
}

/**
 * NOTE: Normally we don't have to wrap __system_property_get (libc.so) as it is only used
 * through the property_get (libcutils.so) function. However when property_get is used
 * internally in libcutils.so we don't have any chance to hook our replacement in.
 * Therefore we have to hook __system_property_get too and just replace it with the
 * implementation of our internal property handling
 */

int _hybris_hook_system_property_get(const char *name, const char *value)
{
    TRACE_HOOK("name '%s' value '%s'", name, value);

    return property_get(name, (char*) value, NULL);
}

int _hybris_hook_property_get(const char *key, char *value, const char *default_value)
{
    TRACE_HOOK("key '%s' value '%s' default value '%s'",
               key, value, default_value);

    return property_get(key, value, default_value);
}

int _hybris_hook_property_set(const char *key, const char *value)
{
    TRACE_HOOK("key '%s' value '%s'", key, value);

    return property_set(key, value);
}

char *_hybris_hook_getenv(const char *name)
{
    TRACE_HOOK("name '%s'", name);

    return getenv(name);
}

int _hybris_hook_setenv(const char *name, const char *value, int overwrite)
{
    TRACE_HOOK("name '%s' value '%s' overwrite %d", name, value, overwrite);

    return setenv(name, value, overwrite);
}

int _hybris_hook_putenv(char *string)
{
    TRACE_HOOK("string '%s'", string);

    return putenv(string);
}

int _hybris_hook_clearenv(void)
{
    TRACE_HOOK("");

    return clearenv();
}

extern int __cxa_atexit(void (*)(void*), void*, void*);
extern void __cxa_finalize(void * d);

struct open_redirect {
    const char *from;
    const char *to;
};

struct open_redirect open_redirects[] = {
    { "/dev/log/main", "/dev/alog/main" },
    { "/dev/log/radio", "/dev/alog/radio" },
    { "/dev/log/system", "/dev/alog/system" },
    { "/dev/log/events", "/dev/alog/events" },
    { NULL, NULL }
};

int _hybris_hook_open(const char *pathname, int flags, ...)
{
    va_list ap;
    mode_t mode = 0;
    const char *target_path = pathname;

    TRACE_HOOK("pathname '%s' flags %d", pathname, flags);

    if (pathname != NULL) {
            struct open_redirect *entry = &open_redirects[0];
            while (entry->from != NULL) {
                    if (strcmp(pathname, entry->from) == 0) {
                            target_path = entry->to;
                            break;
                    }
                    entry++;
            }
    }

    if (flags & O_CREAT) {
            va_start(ap, flags);
            mode = va_arg(ap, mode_t);
            va_end(ap);
    }

    return open(target_path, flags, mode);
}

/**
 * Wrap some GCC builtin functions, which don't have any address
 */
__THROW int my__sprintf_chk (char *__restrict __s, int __flag, size_t __slen,
			  const char *__restrict __format, ...)
{
    int ret = 0;
    va_list args;
    va_start(args,__format);
    ret = __vsprintf_chk (__s, __flag, __slen, __format, args);
    va_end(args);

    return ret;
}
__THROW int my__snprintf_chk (char *__restrict __s, size_t __n, int __flag,
			   size_t __slen, const char *__restrict __format, ...)
{
    int ret = 0;
    va_list args;
    va_start(args,__format);
    ret = __vsnprintf_chk (__s, __n, __flag, __slen, __format, args);
    va_end(args);

    return ret;
}

static __thread void *tls_hooks[16];

static void *_hybris_hook_get_tls_hooks()
{
    TRACE_HOOK("");
    return tls_hooks;
}

int _hybris_hook_prctl(int option, unsigned long arg2, unsigned long arg3,
             unsigned long arg4, unsigned long arg5)
{
    TRACE_HOOK("option %d arg2 %lu arg3 %lu arg4 %lu arg5 %lu",
               option, arg2, arg3, arg4, arg5);

#ifdef MALI_QUIRKS
    if (option == PR_SET_NAME) {
        char *name = (char*) arg2;

        if (strcmp(name, MALI_HIST_DUMP_THREAD_NAME) == 0) {

            // This can only work because prctl with PR_SET_NAME
            // can be only called for the current thread and not
            // for another thread so we can safely pause things.

            HYBRIS_DEBUG_LOG(HOOKS, "%s: Found mali-hist-dump, killing thread ...",
                             __FUNCTION__);

            pthread_exit(NULL);
        }
    }
#endif

    return prctl(option, arg2, arg3, arg4, arg5);
}

static char* _hybris_hook_basename(const char *path)
{
    static __thread char buf[PATH_MAX];

    TRACE_HOOK("path '%s'", path);

    memset(buf, 0, sizeof(buf));

    if (path)
        strncpy(buf, path, sizeof(buf));

    buf[sizeof buf - 1] = '\0';

    return basename(buf);
}

static char* _hybris_hook_dirname(char *path)
{
    static __thread char buf[PATH_MAX];

    TRACE_HOOK("path '%s'", path);

    memset(buf, 0, sizeof(buf));

    if (path)
        strncpy(buf, path, sizeof(buf));

    buf[sizeof buf - 1] = '\0';

    return dirname(path);
}

static char* _hybris_hook_strerror(int errnum)
{
    TRACE_HOOK("errnum %d", errnum);

    return strerror(errnum);
}

static char* _hybris_hook__gnu_strerror_r(int errnum, char *buf, size_t buf_len)
{
    TRACE_HOOK("errnum %d buf '%s' buf len %zu", errnum, buf, buf_len);

    return strerror_r(errnum, buf, buf_len);
}

static int _hybris_hook_mprotect(void *addr, size_t len, int prot)
{
    TRACE_HOOK("addr %p len %zu prot %d", addr, len, prot);

    return mprotect(addr, len, prot);
}

static int _hybris_hook_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    TRACE_HOOK("memptr %p alignment %zu size %zu", memptr, alignment, size);

    return posix_memalign(memptr, alignment, size);
}

static pid_t _hybris_hook_fork(void)
{
    TRACE_HOOK("");

    return fork();
}

static locale_t _hybris_hook_newlocale(int category_mask, const char *locale, locale_t base)
{
    TRACE_HOOK("category mask %i locale '%s'", category_mask, locale);

    return newlocale(category_mask, locale, base);
}

static void _hybris_hook_freelocale(locale_t locobj)
{
    TRACE_HOOK("");

    return freelocale(locobj);
}

static locale_t _hybris_hook_duplocale(locale_t locobj)
{
    TRACE_HOOK("");

    return duplocale(locobj);
}

static locale_t _hybris_hook_uselocale(locale_t newloc)
{
    TRACE_HOOK("");

    return uselocale(newloc);
}

static struct lconv* _hybris_hook_localeconv(void)
{
    TRACE_HOOK("");

    return localeconv();
}

static char* _hybris_hook_setlocale(int category, const char *locale)
{
    TRACE_HOOK("category %i locale '%s'", category, locale);

    return setlocale(category, locale);
}

static void* _hybris_hook_mmap(void *addr, size_t len, int prot,
                  int flags, int fd, off_t offset)
{
    TRACE_HOOK("addr %p len %zu prot %i flags %i fd %i offset %jd",
               addr, len, prot, flags, fd, offset);

    return mmap(addr, len, prot, flags, fd, offset);
}

static int _hybris_hook_munmap(void *addr, size_t length)
{
    TRACE_HOOK("addr %p length %zu", addr, length);

    return munmap(addr, length);
}

extern size_t strlcat(char *dst, const char *src, size_t siz);
extern size_t strlcpy(char *dst, const char *src, size_t siz);

static int _hybris_hook_strcmp(const char *s1, const char *s2)
{
    TRACE_HOOK("s1 '%s' s2 '%s'", s1, s2);

    if ( s1 == NULL || s2 == NULL)
        return -1;

    return strcmp(s1, s2);
}

static FILE* _hybris_hook_setmntent(const char *filename, const char *type)
{
    TRACE_HOOK("filename %s type %s", filename, type);

    return setmntent(filename, type);
}

static struct mntent* _hybris_hook_getmntent(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    /* glibc doesn't allow NULL fp here, but bionic does. */
    if (fp == NULL)
        return NULL;

    return getmntent(_get_actual_fp(fp));
}

static struct mntent* _hybris_hook_getmntent_r(FILE *fp, struct mntent *e, char *buf, int buf_len)
{
    TRACE_HOOK("fp %p e %p buf '%s' buf len %i",
               fp, e, buf, buf_len);

    /* glibc doesn't allow NULL fp here, but bionic does. */
    if (fp == NULL)
        return NULL;

    return getmntent_r(_get_actual_fp(fp), e, buf, buf_len);
}

int _hybris_hook_endmntent(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return endmntent(_get_actual_fp(fp));
}

static int _hybris_hook_fputws(const wchar_t *ws, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fputws(ws, _get_actual_fp(stream));
}

static int _hybris_hook_vfwprintf(FILE *stream, const wchar_t *format, va_list args)
{
    TRACE_HOOK("stream %p", stream);

    return vfwprintf(_get_actual_fp(stream), format, args);
}

static wint_t _hybris_hook_fputwc(wchar_t wc, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fputwc(wc, _get_actual_fp(stream));
}

static wint_t _hybris_hook_putwc(wchar_t wc, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return putwc(wc, _get_actual_fp(stream));
}

static wint_t _hybris_hook_fgetwc(FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fgetwc(_get_actual_fp(stream));
}

static wint_t _hybris_hook_getwc(FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return getwc(_get_actual_fp(stream));
}

static void *_hybris_hook_android_dlopen(const char *filename, int flag)
{
    TRACE("filename %s flag %i", filename, flag);

    return _android_dlopen(filename,flag);
}

static void *_hybris_hook_android_dlsym(void *handle, const char *symbol)
{
    TRACE("handle %p symbol %s", handle, symbol);

    return _android_dlsym(handle,symbol);
}

static void* _hybris_hook_android_dladdr(void *addr, Dl_info *info)
{
    TRACE("addr %p info %p", addr, info);

    return _android_dladdr(addr, info);
}

static int _hybris_hook_android_dlclose(void *handle)
{
    TRACE("handle %p", handle);

    return _android_dlclose(handle);
}

static const char *_hybris_hook_android_dlerror(void)
{
    TRACE("");

    return android_dlerror();
}

static struct _hook hooks_common[] = {
    {"property_get", _hybris_hook_property_get },
    {"property_set", _hybris_hook_property_set },
    {"__system_property_get", _hybris_hook_system_property_get },
    {"getenv", _hybris_hook_getenv},
    {"printf", printf },
    {"malloc", _hybris_hook_malloc },
    {"free", free },
    {"calloc", calloc },
    {"cfree", cfree },
    {"realloc", realloc },
    {"memalign", memalign },
    {"valloc", valloc },
    {"pvalloc", pvalloc },
    {"fread", fread },
    {"getxattr", getxattr},
    {"mprotect", _hybris_hook_mprotect},
    /* string.h */
    {"memccpy",memccpy},
    {"memchr",memchr},
    {"memrchr",memrchr},
    {"memcmp",_hybris_hook_memcmp},
    {"memcpy",_hybris_hook_memcpy},
    {"memmove",memmove},
    {"memset",memset},
    {"memmem",memmem},
    {"getlogin", getlogin},
    // {"memswap",memswap},
    {"index",index},
    {"rindex",rindex},
    {"strchr",strchr},
    {"strrchr",strrchr},
    {"strlen",_hybris_hook_strlen},
    {"strcmp",_hybris_hook_strcmp},
    {"strcpy",strcpy},
    {"strcat",strcat},
    {"strcasecmp",strcasecmp},
    {"strncasecmp",strncasecmp},
    {"strdup",strdup},
    {"strstr",strstr},
    {"strtok",strtok},
    {"strtok_r",strtok_r},
    {"strerror",_hybris_hook_strerror},
    {"strerror_r",strerror_r},
    {"strnlen",strnlen},
    {"strncat",strncat},
    {"strndup",strndup},
    {"strncmp",strncmp},
    {"strncpy",strncpy},
    {"strtod", _hybris_hook_strtod},
    {"strcspn",strcspn},
    {"strpbrk",strpbrk},
    {"strsep",strsep},
    {"strspn",strspn},
    {"strsignal",strsignal},
    {"getgrnam", getgrnam},
    {"strcoll",strcoll},
    {"strxfrm",strxfrm},
    /* strings.h */
    {"bcmp",bcmp},
    {"bcopy",bcopy},
    {"bzero",bzero},
    {"ffs",ffs},
    {"index",index},
    {"rindex",rindex},
    {"strcasecmp",strcasecmp},
    {"__sprintf_chk", my__sprintf_chk},
    {"__snprintf_chk", my__snprintf_chk},
    {"strncasecmp",strncasecmp},
    /* dirent.h */
    {"opendir", opendir},
    {"closedir", closedir},
    /* pthread.h */
    {"getauxval", getauxval},
    {"gettid", _hybris_hook_gettid},
    {"getpid", getpid},
    {"pthread_atfork", pthread_atfork},
    {"pthread_create", _hybris_hook_pthread_create},
    {"pthread_kill", _hybris_hook_pthread_kill},
    {"pthread_exit", pthread_exit},
    {"pthread_join", pthread_join},
    {"pthread_detach", pthread_detach},
    {"pthread_self", pthread_self},
    {"pthread_equal", pthread_equal},
    {"pthread_getschedparam", pthread_getschedparam},
    {"pthread_setschedparam", pthread_setschedparam},
    {"pthread_mutex_init", _hybris_hook_pthread_mutex_init},
    {"pthread_mutex_destroy", _hybris_hook_pthread_mutex_destroy},
    {"pthread_mutex_lock", _hybris_hook_pthread_mutex_lock},
    {"pthread_mutex_unlock", _hybris_hook_pthread_mutex_unlock},
    {"pthread_mutex_trylock", _hybris_hook_pthread_mutex_trylock},
    {"pthread_mutex_lock_timeout_np", _hybris_hook_pthread_mutex_lock_timeout_np},
    {"pthread_mutex_timedlock", _hybris_hook_pthread_mutex_timedlock},
    {"pthread_mutexattr_init", pthread_mutexattr_init},
    {"pthread_mutexattr_destroy", pthread_mutexattr_destroy},
    {"pthread_mutexattr_gettype", pthread_mutexattr_gettype},
    {"pthread_mutexattr_settype", pthread_mutexattr_settype},
    {"pthread_mutexattr_getpshared", pthread_mutexattr_getpshared},
    {"pthread_mutexattr_setpshared", _hybris_hook_pthread_mutexattr_setpshared},
    {"pthread_condattr_init", pthread_condattr_init},
    {"pthread_condattr_getpshared", pthread_condattr_getpshared},
    {"pthread_condattr_setpshared", pthread_condattr_setpshared},
    {"pthread_condattr_destroy", pthread_condattr_destroy},
    {"pthread_condattr_getclock", pthread_condattr_getclock},
    {"pthread_condattr_setclock", pthread_condattr_setclock},
    {"pthread_cond_init", _hybris_hook_pthread_cond_init},
    {"pthread_cond_destroy", _hybris_hook_pthread_cond_destroy},
    {"pthread_cond_broadcast", _hybris_hook_pthread_cond_broadcast},
    {"pthread_cond_signal", _hybris_hook_pthread_cond_signal},
    {"pthread_cond_wait", _hybris_hook_pthread_cond_wait},
    {"pthread_cond_timedwait", _hybris_hook_pthread_cond_timedwait},
    {"pthread_cond_timedwait_monotonic", _hybris_hook_pthread_cond_timedwait},
    {"pthread_cond_timedwait_monotonic_np", _hybris_hook_pthread_cond_timedwait},
    {"pthread_cond_timedwait_relative_np", _hybris_hook_pthread_cond_timedwait_relative_np},
    {"pthread_key_delete", pthread_key_delete},
    {"pthread_setname_np", _hybris_hook_pthread_setname_np},
    {"pthread_once", pthread_once},
    {"pthread_key_create", pthread_key_create},
    {"pthread_setspecific", _hybris_hook_pthread_setspecific},
    {"pthread_getspecific", _hybris_hook_pthread_getspecific},
    {"pthread_attr_init", _hybris_hook_pthread_attr_init},
    {"pthread_attr_destroy", _hybris_hook_pthread_attr_destroy},
    {"pthread_attr_setdetachstate", _hybris_hook_pthread_attr_setdetachstate},
    {"pthread_attr_getdetachstate", _hybris_hook_pthread_attr_getdetachstate},
    {"pthread_attr_setschedpolicy", _hybris_hook_pthread_attr_setschedpolicy},
    {"pthread_attr_getschedpolicy", _hybris_hook_pthread_attr_getschedpolicy},
    {"pthread_attr_setschedparam", _hybris_hook_pthread_attr_setschedparam},
    {"pthread_attr_getschedparam", _hybris_hook_pthread_attr_getschedparam},
    {"pthread_attr_setstacksize", _hybris_hook_pthread_attr_setstacksize},
    {"pthread_attr_getstacksize", _hybris_hook_pthread_attr_getstacksize},
    {"pthread_attr_setstackaddr", _hybris_hook_pthread_attr_setstackaddr},
    {"pthread_attr_getstackaddr", _hybris_hook_pthread_attr_getstackaddr},
    {"pthread_attr_setstack", _hybris_hook_pthread_attr_setstack},
    {"pthread_attr_getstack", _hybris_hook_pthread_attr_getstack},
    {"pthread_attr_setguardsize", _hybris_hook_pthread_attr_setguardsize},
    {"pthread_attr_getguardsize", _hybris_hook_pthread_attr_getguardsize},
    {"pthread_attr_setscope", _hybris_hook_pthread_attr_setscope},
    {"pthread_attr_getscope", _hybris_hook_pthread_attr_getscope},
    {"pthread_getattr_np", _hybris_hook_pthread_getattr_np},
    {"pthread_rwlockattr_init", _hybris_hook_pthread_rwlockattr_init},
    {"pthread_rwlockattr_destroy", _hybris_hook_pthread_rwlockattr_destroy},
    {"pthread_rwlockattr_setpshared", _hybris_hook_pthread_rwlockattr_setpshared},
    {"pthread_rwlockattr_getpshared", _hybris_hook_pthread_rwlockattr_getpshared},
    {"pthread_rwlock_init", _hybris_hook_pthread_rwlock_init},
    {"pthread_rwlock_destroy", _hybris_hook_pthread_rwlock_destroy},
    {"pthread_rwlock_unlock", _hybris_hook_pthread_rwlock_unlock},
    {"pthread_rwlock_wrlock", _hybris_hook_pthread_rwlock_wrlock},
    {"pthread_rwlock_rdlock", _hybris_hook_pthread_rwlock_rdlock},
    {"pthread_rwlock_tryrdlock", _hybris_hook_pthread_rwlock_tryrdlock},
    {"pthread_rwlock_trywrlock", _hybris_hook_pthread_rwlock_trywrlock},
    {"pthread_rwlock_timedrdlock", _hybris_hook_pthread_rwlock_timedrdlock},
    {"pthread_rwlock_timedwrlock", _hybris_hook_pthread_rwlock_timedwrlock},
    /* bionic-only pthread */
    {"__pthread_gettid", _hybris_hook_pthread_gettid},
    {"pthread_gettid_np", _hybris_hook_pthread_gettid},
    /* stdio.h */
    {"__isthreaded", &___hybris_hook_isthreaded},
    {"__sF", &_hybris_hook_sF},
    {"fopen", fopen},
    {"fdopen", fdopen},
    {"popen", popen},
    {"puts", puts},
    {"sprintf", sprintf},
    {"asprintf", asprintf},
    {"vasprintf", vasprintf},
    {"snprintf", snprintf},
    {"vsprintf", vsprintf},
    {"vsnprintf", vsnprintf},
    {"clearerr", _hybris_hook_clearerr},
    {"fclose", _hybris_hook_fclose},
    {"feof", _hybris_hook_feof},
    {"ferror", _hybris_hook_ferror},
    {"fflush", _hybris_hook_fflush},
    {"fgetc", _hybris_hook_fgetc},
    {"fgetpos", _hybris_hook_fgetpos},
    {"fgets", _hybris_hook_fgets},
    {"fprintf", _hybris_hook_fprintf},
    {"fputc", _hybris_hook_fputc},
    {"fputs", _hybris_hook_fputs},
    {"fread", _hybris_hook_fread},
    {"freopen", _hybris_hook_freopen},
    {"fscanf", _hybris_hook_fscanf},
    {"fseek", _hybris_hook_fseek},
    {"fseeko", _hybris_hook_fseeko},
    {"fsetpos", _hybris_hook_fsetpos},
    {"ftell", _hybris_hook_ftell},
    {"ftello", _hybris_hook_ftello},
    {"fwrite", _hybris_hook_fwrite},
    {"getc", _hybris_hook_getc},
    {"getdelim", _hybris_hook_getdelim},
    {"getline", _hybris_hook_getline},
    {"putc", _hybris_hook_putc},
    {"rewind", _hybris_hook_rewind},
    {"setbuf", _hybris_hook_setbuf},
    {"setvbuf", _hybris_hook_setvbuf},
    {"ungetc", _hybris_hook_ungetc},
    {"vasprintf", vasprintf},
    {"vfprintf", _hybris_hook_vfprintf},
    {"vfscanf", _hybris_hook_vfscanf},
    {"fileno", _hybris_hook_fileno},
    {"pclose", _hybris_hook_pclose},
    {"flockfile", _hybris_hook_flockfile},
    {"ftrylockfile", _hybris_hook_ftrylockfile},
    {"funlockfile", _hybris_hook_funlockfile},
    {"getc_unlocked", _hybris_hook_getc_unlocked},
    {"putc_unlocked", _hybris_hook_putc_unlocked},
    //{"fgetln", _hybris_hook_fgetln},
    {"fpurge", _hybris_hook_fpurge},
    {"getw", _hybris_hook_getw},
    {"putw", _hybris_hook_putw},
    {"setbuffer", _hybris_hook_setbuffer},
    {"setlinebuf", _hybris_hook_setlinebuf},
    {"__errno", __errno_location},
    {"__set_errno", _hybris_hook_set_errno},
    {"__progname", &program_invocation_name},
    /* net specifics, to avoid __res_get_state */
    {"getaddrinfo", _hybris_hook_getaddrinfo},
    {"freeaddrinfo", _hybris_hook_freeaddrinfo},
    {"gethostbyaddr", gethostbyaddr},
    {"gethostbyname", gethostbyname},
    {"gethostbyname2", gethostbyname2},
    {"gethostent", gethostent},
    {"strftime", strftime},
    {"sysconf", _hybris_hook_sysconf},
    {"dlopen", _hybris_hook_android_dlopen},
    {"dlerror", _hybris_hook_android_dlerror},
    {"dlsym", _hybris_hook_android_dlsym},
    {"dladdr", _hybris_hook_android_dladdr},
    {"dlclose", _hybris_hook_android_dlclose},
    /* dirent.h */
    {"opendir", opendir},
    {"fdopendir", fdopendir},
    {"closedir", closedir},
    {"readdir", _hybris_hook_readdir},
    {"readdir_r", _hybris_hook_readdir_r},
    {"rewinddir", rewinddir},
    {"seekdir", seekdir},
    {"telldir", telldir},
    {"dirfd", dirfd},
    {"scandir", my_scandir},
    {"scandirat", my_scandirat},
    {"alphasort", _hybris_hook_alphasort},
    {"versionsort", my_versionsort},
    /* fcntl.h */
    {"open", _hybris_hook_open},
    // TODO: scandir, scandirat, alphasort, versionsort
    {"__get_tls_hooks", _hybris_hook_get_tls_hooks},
    {"sscanf", sscanf},
    {"scanf", scanf},
    {"vscanf", vscanf},
    {"vsscanf", vsscanf},
    {"openlog", openlog},
    {"syslog", syslog},
    {"closelog", closelog},
    {"vsyslog", vsyslog},
    {"timer_create", timer_create},
    {"timer_settime", timer_settime},
    {"timer_gettime", timer_gettime},
    {"timer_delete", timer_delete},
    {"timer_getoverrun", timer_getoverrun},
    {"localtime", localtime},
    {"localtime_r", localtime_r},
    {"gmtime", gmtime},
    {"abort", abort},
    {"writev", writev},
    /* unistd.h */
    {"access", access},
    /* grp.h */
    {"getgrgid", getgrgid},
    {"__cxa_atexit", __cxa_atexit},
    {"__cxa_finalize", __cxa_finalize},
    {"__system_property_read", ___hybris_hook_system_property_read},
    {"__system_property_set", property_set},
    {"__system_property_foreach", ___hybris_hook_system_property_foreach},
    {"__system_property_find", ___hybris_hook_system_property_find},
    {"__system_property_serial", ___hybris_hook_system_property_serial},
    {"__system_property_wait", ___hybris_hook_system_property_wait},
    {"__system_property_update", ___hybris_hook_system_property_update},
    {"__system_property_add", ___hybris_hook_system_property_add},
    {"__system_property_wait_any", ___hybris_hook_system_property_wait_any},
    {"__system_property_find_nth", ___hybris_hook_system_property_find_nth},
    /* sys/prctl.h */
    {"prctl", _hybris_hook_prctl},
};

static struct _hook hooks_mm[] = {
    {"strtol", _hybris_hook_strtol},
    {"strlcat",strlcat},
    {"strlcpy",strlcpy},
    {"setenv", _hybris_hook_setenv},
    {"putenv", _hybris_hook_putenv},
    {"clearenv", _hybris_hook_clearenv},
    {"dprintf", dprintf},
    {"mallinfo", mallinfo},
    {"malloc_usable_size", _hybris_hook_malloc_usable_size},
    {"posix_memalign", _hybris_hook_posix_memalign},
    {"mprotect", _hybris_hook_mprotect},
    {"__gnu_strerror_r",_hybris_hook__gnu_strerror_r},
    {"pthread_rwlockattr_getkind_np", _hybris_hook_pthread_rwlockattr_getkind_np},
    {"pthread_rwlockattr_setkind_np", _hybris_hook_pthread_rwlockattr_setkind_np},
    /* unistd.h */
    {"fork", _hybris_hook_fork},
    {"ttyname", ttyname},
    {"swprintf", swprintf},
    {"fmemopen", fmemopen},
    {"open_memstream", open_memstream},
    {"open_wmemstream", open_wmemstream},
    {"ptsname", ptsname},
    {"__hybris_set_errno_internal", _hybris_hook_set_errno},
    {"getservbyname", getservbyname},
    /* libgen.h */
    {"basename", _hybris_hook_basename},
    {"dirname", _hybris_hook_dirname},
    /* locale.h */
    {"newlocale", _hybris_hook_newlocale},
    {"freelocale", _hybris_hook_freelocale},
    {"duplocale", _hybris_hook_duplocale},
    {"uselocale", _hybris_hook_uselocale},
    {"localeconv", _hybris_hook_localeconv},
    {"setlocale", _hybris_hook_setlocale},
    /* sys/mman.h */
    {"mmap", _hybris_hook_mmap},
    {"munmap", _hybris_hook_munmap},
    /* wchar.h */
    {"wmemchr", wmemchr},
    {"wmemcmp", wmemcmp},
    {"wmemcpy", wmemcpy},
    {"wmemmove", wmemmove},
    {"wmemset", wmemset},
    {"wmempcpy", wmempcpy},
    {"fputws", _hybris_hook_fputws},
    // It's enough to hook vfwprintf here as fwprintf will call it with a
    // proper va_list in place so we don't have to handle this here.
    {"vfwprintf", _hybris_hook_vfwprintf},
    {"fputwc", _hybris_hook_fputwc},
    {"putwc", _hybris_hook_putwc},
    {"fgetwc", _hybris_hook_fgetwc},
    {"getwc", _hybris_hook_getwc},
    /* sched.h */
    {"clone", clone},
    /* mntent.h */
    {"setmntent", _hybris_hook_setmntent},
    {"getmntent", _hybris_hook_getmntent},
    {"getmntent_r", _hybris_hook_getmntent_r},
    {"endmntent", _hybris_hook_endmntent},
    /* stdlib.h */
    {"system", system},
    /* pwd.h */
    {"getgrnam", getgrnam},
    {"getpwuid", getpwuid},
    {"getpwnam", getpwnam},
    /* signal.h */
    /* Hooks commented out for the moment as we need proper translations between
     * bionic and glibc types for them to work (for instance, sigset_t has
     * different definitions in each library).
     */
#if 0
    {"sigaction", sigaction},
    {"sigaddset", sigaddset},
    {"sigaltstack", sigaltstack},
    {"sigblock", sigblock},
    {"sigdelset", sigdelset},
    {"sigemptyset", sigemptyset},
    {"sigfillset", sigfillset},
    {"siginterrupt", siginterrupt},
    {"sigismember", sigismember},
    {"siglongjmp", siglongjmp},
    {"signal", signal},
    {"signalfd", signalfd},
    {"sigpending", sigpending},
    {"sigprocmask", sigprocmask},
    {"sigqueue", sigqueue},
    // setjmp.h defines segsetjmp via a #define and the real symbol
    // we have to forward to is __sigsetjmp
    {"sigsetjmp", __sigsetjmp},
    {"sigsetmask", sigsetmask},
    {"sigsuspend", sigsuspend},
    {"sigtimedwait", sigtimedwait},
    {"sigwait", sigwait},
    {"sigwaitinfo", sigwaitinfo},
#endif
    /* dirent.h */
    {"readdir64", _hybris_hook_readdir},
    {"readdir64_r", _hybris_hook_readdir_r},
    {"scandir", _hybris_hook_scandir},
    {"scandirat", _hybris_hook_scandirat},
    {"alphasort,", _hybris_hook_alphasort},
    {"versionsort,", _hybris_hook_versionsort},
    {"scandir64", _hybris_hook_scandir},
};


static int hook_cmp(const void *a, const void *b)
{
    return strcmp(((struct _hook*)a)->name, ((struct _hook*)b)->name);
}

void hybris_set_hook_callback(hybris_hook_cb callback)
{
    hook_callback = callback;
}

static int get_android_sdk_version()
{
    static int sdk_version = -1;

    if (sdk_version > 0)
        return sdk_version;

    char value[PROP_VALUE_MAX];
    property_get("ro.build.version.sdk", value, "19");

    sdk_version = 19;
    if (strlen(value) > 0)
        sdk_version = atoi(value);

#ifdef UBUNTU_LINKER_OVERRIDES
    /* We override both frieza and turbo here until they are ready to be
     * upgraded to the newer linker. */
    char device_name[PROP_VALUE_MAX];
    memset(device_name, 0, sizeof(device_name));
    property_get("ro.build.product", device_name, "");
    if (strlen(device_name) > 0) {
        /* Force SDK version for both frieza/cooler and turbo for the time being */
        if (strcmp(device_name, "frieza") == 0 ||
            strcmp(device_name, "cooler") == 0 ||
            strcmp(device_name, "turbo") == 0)
            sdk_version = 19;
    }
#endif

    char *version_override = getenv("HYBRIS_ANDROID_SDK_VERSION");
    if (version_override)
        sdk_version = atoi(version_override);

    LOGD("Using SDK API version %i\n", sdk_version);

    return sdk_version;
}

#define HOOKS_SIZE(hooks) \
    (sizeof(hooks) / sizeof(hooks[0]))


static void* __hybris_get_hooked_symbol(const char *sym, const char *requester)
{
    static int sorted = 0;
    static intptr_t counter = -1;
    void *found = NULL;
    struct _hook key;

    /* First check if we have a callback registered which could
     * give us a context specific hook implementation */
    if (hook_callback)
    {
        found = hook_callback(sym, requester);
        if (found)
            return (void*) found;
    }

    if (!sorted)
    {
        qsort(hooks_common, HOOKS_SIZE(hooks_common), sizeof(hooks_common[0]), hook_cmp);
        qsort(hooks_mm, HOOKS_SIZE(hooks_mm), sizeof(hooks_mm[0]), hook_cmp);
        sorted = 1;
    }

    /* Allow newer hooks to override those which are available for all versions */
    key.name = sym;
    if (get_android_sdk_version() > 21)
        found = bsearch(&key, hooks_mm, HOOKS_SIZE(hooks_mm), sizeof(hooks_mm[0]), hook_cmp);
    if (!found)
        found = bsearch(&key, hooks_common, HOOKS_SIZE(hooks_common), sizeof(hooks_common[0]), hook_cmp);

    if (found)
        return ((struct _hook*) found)->func;

    if (strncmp(sym, "pthread", 7) == 0 ||
        strncmp(sym, "__pthread", 9) == 0)
    {
        /* safe */
        if (strcmp(sym, "pthread_sigmask") == 0)
           return NULL;
        /* not safe */
        counter--;
        // If you're experiencing a crash later on check the address of the
        // function pointer being call. If it matches the printed counter
        // value here then you can easily find out which symbol is missing.
        LOGD("Missing hook for pthread symbol %s (counter %" PRIiPTR ")\n", sym, counter);
        return (void *) counter;
    }

    if (!getenv("HYBRIS_DONT_PRINT_SYMBOLS_WITHOUT_HOOK"))
        LOGD("Could not find a hook for symbol %s", sym);

    return NULL;
}

static void *linker_handle = NULL;

static void* __hybris_load_linker(const char *path)
{
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "ERROR: Failed to load hybris linker for Android SDK version %d\n",
                get_android_sdk_version());
        return NULL;
    }
    return handle;
}

#define LINKER_NAME_JB "jb"
#define LINKER_NAME_MM "mm"

#if defined(WANT_LINKER_JB)
#define LINKER_NAME_DEFAULT LINKER_NAME_JB
#elif defined(WANT_LINKER_MM)
#define LINKER_NAME_DEFAULT LINKER_NAME_MM
#endif

static int linker_initialized = 0;

static void __hybris_linker_init()
{
    LOGD("Linker initialization");

    int sdk_version = get_android_sdk_version();

    char path[PATH_MAX];
    const char *name = LINKER_NAME_DEFAULT;

    /* See https://source.android.com/source/build-numbers.html for
     * an overview over available SDK version numbers and which
     * Android version they relate to. */
#if defined(WANT_LINKER_MM)
    if (sdk_version <= 23)
        name = LINKER_NAME_MM;
#endif
#if defined(WANT_LINKER_JB)
    if (sdk_version < 21)
        name = LINKER_NAME_JB;
#endif

    const char *linker_dir = LINKER_PLUGIN_DIR;
    const char *user_linker_dir = getenv("HYBRIS_LINKER_DIR");
    if (user_linker_dir)
        linker_dir = user_linker_dir;

    snprintf(path, PATH_MAX, "%s/%s.so", linker_dir, name);

    LOGD("Loading linker from %s..", path);

    linker_handle = __hybris_load_linker(path);
    if (!linker_handle)
        exit(1);

    /* Load all necessary symbols we need from the linker */
    _android_linker_init = dlsym(linker_handle, "android_linker_init");
    _android_dlopen = dlsym(linker_handle, "android_dlopen");
    _android_dlsym = dlsym(linker_handle, "android_dlsym");
    _android_dladdr = dlsym(linker_handle, "android_dladdr");
    _android_dlclose = dlsym(linker_handle, "android_dlclose");
    _android_dlerror = dlsym(linker_handle, "android_dlerror");

    /* Now its time to setup the linker itself */
    _android_linker_init(sdk_version, __hybris_get_hooked_symbol);

    linker_initialized = 1;
}

#define ENSURE_LINKER_IS_LOADED() \
    if (!linker_initialized) \
        __hybris_linker_init();

/* NOTE: As we're not linking directly with the linker anymore
 * but several users are using android_* functions directly we
 * have to export them here. */

void *android_dlopen(const char *filename, int flag)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlopen)
        return NULL;

    return _android_dlopen(filename,flag);
}

void *android_dlsym(void *handle, const char *symbol)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlsym)
        return NULL;

    return _android_dlsym(handle,symbol);
}

int android_dlclose(void *handle)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlclose)
        return -1;

    return _android_dlclose(handle);
}

const char *android_dlerror(void)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlerror)
        return NULL;

    return _android_dlerror();
}

void *hybris_dlopen(const char *filename, int flag)
{
    return android_dlopen(filename,flag);
}

void *hybris_dlsym(void *handle, const char *symbol)
{
    return android_dlsym(handle,symbol);
}

int hybris_dlclose(void *handle)
{
    return android_dlclose(handle);
}

const char *hybris_dlerror(void)
{
    return android_dlerror();
}

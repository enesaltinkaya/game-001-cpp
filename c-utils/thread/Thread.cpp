#include "Thread.h"
#include <algorithm>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "platform/Platform.h"
#include "thpool/thpool.h"
#include "logger/Logger.h"

namespace utils {
static pthread_key_t threadLocalNameKey;

Thread* threadNew(FnPtrPtr fn, void* userData) {
    Thread* thread = new Thread{};
    pthread_create(&thread->thread, 0, fn, userData);
    return thread;
}

void threadJoin(Thread* thread) {
    pthread_join(thread->thread, 0);
}

void threadDestroy(Thread* thread) {
    delete thread;
}

void threadNewDetached(FnPtrPtr fn, void* userData) {
    pthread_t thread = {};
    pthread_create(&thread, 0, fn, userData);
    pthread_detach(thread);
}

void threadLock(Thread* thread) {
#ifndef NDEBUG
    int result = pthread_mutex_lock(&thread->mutex);
    if (result != 0) {
        terminate("ERROR: pthread_mutex_lock failed with %d (%s) at %s:%d\n",
                  result,
                  strerror(result),
                  __FILE__,
                  __LINE__);
    }
#else
    pthread_mutex_lock(&thread->mutex);
#endif
}

void threadUnlock(Thread* thread) {
#ifndef NDEBUG
    int result = pthread_mutex_unlock(&thread->mutex);
    if (result != 0) {
        terminate("ERROR: pthread_mutex_lock failed with %d (%s) at %s:%d\n",
                  result,
                  strerror(result),
                  __FILE__,
                  __LINE__);
    }
#else
    pthread_mutex_unlock(&thread->mutex);
#endif
}

void threadTryLock(Thread* thread) {
    if (!pthread_mutex_trylock(&thread->mutex)) {
        info("pthread_mutex_trylock failed and we stopped the warning");
    }
}

void threadWait(Thread* thread) {
    pthread_cond_wait(&thread->cond, &thread->mutex);
}

void threadSignal(Thread* thread) {
    pthread_cond_signal(&thread->cond);
}

void threadSetName(const char* name) {
    if (threadLocalNameKey == 0) {
        pthread_key_create(&threadLocalNameKey, nullptr);
    }
    pthread_setspecific(threadLocalNameKey, name);
}

const char* threadGetName(void) {
    return static_cast<const char*>(pthread_getspecific(threadLocalNameKey));
}

u32 threadGetId(void) {
    return pthread_self();
}

void threadInitMutex(Thread* thread) {
#ifndef NDEBUG
    pthread_mutexattr_t attr = {};
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&thread->mutex, &attr);
    pthread_mutexattr_destroy(&attr);  // Don't forget to clean up the attributes object
#else
    pthread_mutex_init(&thread->mutex, nullptr);
#endif
}

static ThreadPool* defaultThreadPool;

ThreadPool* threadPoolInit(int poolSize) {
    info("threadPool: initializing");
    int numCores = numberOfCores();
    if (!poolSize) poolSize = std::max(1, numCores - 1);
    ThreadPool* pool = reinterpret_cast<ThreadPool*>(thpool_init(poolSize));
    if (!defaultThreadPool) defaultThreadPool = pool;
    debug("threadPool: cores %d", numCores - 1);
    return pool;
}

void threadPoolDestroy(ThreadPool* pool) {
    if (!pool) pool = defaultThreadPool;
    thpool_destroy(reinterpret_cast<threadpool>(pool));
}

void threadPoolAddWork(ThreadPool* pool, void (*fn)(void*), void* userData) {
    if (!pool) pool = defaultThreadPool;
    thpool_add_work(reinterpret_cast<threadpool>(pool), fn, userData);
}

void threadPoolWait(ThreadPool* pool) {
    if (!pool) pool = defaultThreadPool;
    thpool_wait(reinterpret_cast<threadpool>(pool));
}

void threadRaiseTrap(void) {
#ifndef NDEBUG
#ifdef __linux__
    raise(SIGTRAP);
#endif
#endif
}
}  // namespace utils

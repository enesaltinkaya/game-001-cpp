#pragma once
#include "pthread.h"

namespace utils {
struct Thread {
    pthread_cond_t cond = {};
    pthread_mutex_t mutex;
    pthread_t thread = {};
};

// ThreadPool wrapper for libthpool
struct ThreadPool {
    void* internal;
};

#define THREAD_LOCK                                                                          \
    static utils::Thread thread = {.cond = {}, .mutex = PTHREAD_MUTEX_INITIALIZER, .thread = {}}; \
    utils::threadLock(&thread)
#define THREAD_UNLOCK utils::threadUnlock(&thread)

Thread* threadNew(FnPtrPtr fn, void* userData);
void threadJoin(Thread* thread);
void threadDestroy(Thread* thread);

void threadInitMutex(Thread* thread);

void threadNewDetached(FnPtrPtr fn, void* userData);
void threadLock(Thread* thread);
void threadTryLock(Thread* thread);
void threadUnlock(Thread* thread);
void threadWait(Thread* thread);
void threadSignal(Thread* thread);

void threadSetName(const char* name);
const char* threadGetName(void);
u32 threadGetId(void);

ThreadPool* threadPoolInit(int poolSize);
void threadPoolDestroy(ThreadPool* pool);
void threadPoolAddWork(ThreadPool* pool, void (*fn)(void*), void* userData);
void threadPoolWait(ThreadPool* pool);

void threadRaiseTrap(void);
}  // namespace utils

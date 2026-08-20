#pragma once
#include "pthread.h"

typedef struct Thread {
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    pthread_t thread;
} Thread;

// ThreadPool wrapper for libthpool
typedef struct ThreadPool {
    void* internal;
} ThreadPool;

#define THREAD_LOCK                                              \
    static Thread thread = {.mutex = PTHREAD_MUTEX_INITIALIZER}; \
    threadLock(&thread)
#define THREAD_UNLOCK threadUnlock(&thread)

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

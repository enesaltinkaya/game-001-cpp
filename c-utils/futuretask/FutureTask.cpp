#include "FutureTask.h"
#include "Utils.h"
#include "container/Map.h"
#include "thread/Thread.h"
#include "container/Array.h"

struct TaskEntry {
    double date;
    void (*fn)(void*);
    void (*fnVoid)(void);
    void* userData;
};

static Map(int, TaskEntry) tasks;
static int taskCounter;
static Array(int) removeList = {};

int futureTaskAdd(double millis, FnPtr fn, void* userData) {
    THREAD_LOCK;
    TaskEntry task = {};
    task.fn        = fn;
    task.date      = nanos() + millis * MILLION;
    task.userData  = userData;

    int taskKey = taskCounter;
    taskCounter++;
    mapPut(tasks, taskKey, task);
    THREAD_UNLOCK;
    return taskKey;
}

int futureTaskAddNoParam(double millis, FnVoid fn) {
    THREAD_LOCK;
    TaskEntry task = {};
    task.fnVoid    = fn;
    task.date      = nanos() + millis * MILLION;
    task.userData  = nullptr;

    int taskKey = taskCounter;
    taskCounter++;
    mapPut(tasks, taskKey, task);
    THREAD_UNLOCK;
    return taskKey;
}

void futureTaskRemove(int taskKey) {
    mapRemove(tasks, taskKey);
}

void futureTaskRun(void) {
    for (i32 i = 0, si = mapSize(tasks); i < si; i++) {
        TaskEntry* task = &tasks[i].value;
        if (nanos() > task->date) {
            if (task->fn) {
                task->fn(task->userData);
            } else {
                task->fnVoid();
            }
            arrayPut(removeList, tasks[i].key);
        }
    }

    for (i32 i = 0, si = arraySize(removeList); i < si; i++) {
        mapRemove(tasks, removeList[i]);
    }
    arrayClear(removeList);
}

void futureTaskFinish(void) {
    // Run tasks in the order they were scheduled (FIFO, ascending key).
    // Shutdown depends on this: the Vulkan swapchain must be destroyed before
    // the window system's SDL_Quit closes the X11 display the AMD driver needs
    // for vkDestroySwapchainKHR. Hash-map order is not insertion order, so pick
    // the smallest (earliest-scheduled) key each iteration.
    while (mapSize(tasks) > 0) {
        int minKey = -1;
        for (i32 i = 0, si = mapSize(tasks); i < si; i++) {
            int key = tasks[i].key;
            if (minKey == -1 || key < minKey) minKey = key;
        }
        TaskEntry task = mapGet(tasks, minKey);
        if (task.fn) {
            task.fn(task.userData);
        } else {
            task.fnVoid();
        }
        (void)mapRemove(tasks, minKey);
    }

    mapFree(tasks);
    arrayFree(removeList);
}

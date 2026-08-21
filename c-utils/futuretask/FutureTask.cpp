#include "FutureTask.h"
#include "Utils.h"
#include <unordered_map>
#include "thread/Thread.h"
#include <vector>

namespace utils {
struct TaskEntry {
    double date;
    void (*fn)(void*);
    void (*fnVoid)(void);
    void* userData;
};

static std::unordered_map<int, TaskEntry> tasks;
static int taskCounter;
// Single lock shared by ALL map accessors.  Worker threads call
// futureTaskAdd() while the main thread calls futureTaskRun()/Remove(), so
// every access below must go through this one mutex.
static utils::Thread tasksLock = {.cond = {}, .mutex = PTHREAD_MUTEX_INITIALIZER, .thread = {}};

int futureTaskAdd(double millis, FnPtr fn, void* userData) {
    utils::threadLock(&tasksLock);
    TaskEntry task = {};
    task.fn        = fn;
    task.date      = nanos() + millis * MILLION;
    task.userData  = userData;

    int taskKey = taskCounter;
    taskCounter++;
    tasks[taskKey] = task;
    utils::threadUnlock(&tasksLock);
    return taskKey;
}

int futureTaskAddNoParam(double millis, FnVoid fn) {
    utils::threadLock(&tasksLock);
    TaskEntry task = {};
    task.fnVoid    = fn;
    task.date      = nanos() + millis * MILLION;
    task.userData  = nullptr;

    int taskKey = taskCounter;
    taskCounter++;
    tasks[taskKey] = task;
    utils::threadUnlock(&tasksLock);
    return taskKey;
}

void futureTaskRemove(int taskKey) {
    utils::threadLock(&tasksLock);
    tasks.erase(taskKey);
    utils::threadUnlock(&tasksLock);
}

void futureTaskRun(void) {
    // Snapshot the due tasks under the lock, run their callbacks WITHOUT the
    // lock (callbacks may re-enter futureTaskAdd), then remove the keys.
    std::vector<TaskEntry> due;
    std::vector<int>       dueKeys;
    utils::threadLock(&tasksLock);
    for (auto& [key, task] : tasks) {
        if (nanos() > task.date) {
            due.push_back(task);
            dueKeys.push_back(key);
        }
    }
    for (int key : dueKeys) {
        tasks.erase(key);
    }
    utils::threadUnlock(&tasksLock);

    for (auto& task : due) {
        if (task.fn) {
            task.fn(task.userData);
        } else {
            task.fnVoid();
        }
    }
}

void futureTaskFinish(void) {
    // Run tasks in the order they were scheduled (FIFO, ascending key).
    // Shutdown depends on this: the Vulkan swapchain must be destroyed before
    // the window system's SDL_Quit closes the X11 display the AMD driver needs
    // for vkDestroySwapchainKHR. Hash-map order is not insertion order, so pick
    // the smallest (earliest-scheduled) key each iteration.
    while (true) {
        TaskEntry task = {};
        bool      found = false;
        utils::threadLock(&tasksLock);
        if (!tasks.empty()) {
            int minKey = -1;
            for (const auto& entry : tasks) {
                if (minKey == -1 || entry.first < minKey) minKey = entry.first;
            }
            auto it = tasks.find(minKey);
            task    = it->second;
            tasks.erase(it);
            found   = true;
        }
        utils::threadUnlock(&tasksLock);

        if (!found) break;
        if (task.fn) {
            task.fn(task.userData);
        } else {
            task.fnVoid();
        }
    }
}
}  // namespace utils
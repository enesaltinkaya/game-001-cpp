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
static std::vector<int> removeList = {};

int futureTaskAdd(double millis, FnPtr fn, void* userData) {
    THREAD_LOCK;
    TaskEntry task = {};
    task.fn        = fn;
    task.date      = nanos() + millis * MILLION;
    task.userData  = userData;

    int taskKey = taskCounter;
    taskCounter++;
    tasks[taskKey] = task;
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
    tasks[taskKey] = task;
    THREAD_UNLOCK;
    return taskKey;
}

void futureTaskRemove(int taskKey) {
    tasks.erase(taskKey);
}

void futureTaskRun(void) {
    for (auto& [key, task] : tasks) {
        if (nanos() > task.date) {
            if (task.fn) {
                task.fn(task.userData);
            } else {
                task.fnVoid();
            }
            removeList.push_back(key);
        }
    }

    for (i32 i = 0, si = static_cast<i32>(removeList.size()); i < si; i++) {
        tasks.erase(removeList[i]);
    }
    removeList.clear();
}

void futureTaskFinish(void) {
    // Run tasks in the order they were scheduled (FIFO, ascending key).
    // Shutdown depends on this: the Vulkan swapchain must be destroyed before
    // the window system's SDL_Quit closes the X11 display the AMD driver needs
    // for vkDestroySwapchainKHR. Hash-map order is not insertion order, so pick
    // the smallest (earliest-scheduled) key each iteration.
    while (static_cast<i32>(tasks.size()) > 0) {
        int minKey = -1;
        for (const auto& entry : tasks) {
            if (minKey == -1 || entry.first < minKey) minKey = entry.first;
        }
        auto it = tasks.find(minKey);
        TaskEntry task = it != tasks.end() ? it->second : TaskEntry{};
        if (task.fn) {
            task.fn(task.userData);
        } else {
            task.fnVoid();
        }
        (void)tasks.erase(minKey);
    }

}
}  // namespace utils

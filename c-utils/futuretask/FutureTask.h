#pragma once

namespace utils {
int futureTaskAdd(double millis, FnPtr fn, void* userData);
void futureTaskRemove(int taskKey);

int futureTaskAddNoParam(double millis, void (*fn)(void));
void futureTaskRun(void);
void futureTaskFinish(void);
}  // namespace utils

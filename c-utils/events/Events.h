#pragma once

namespace utils {
void signalSubscribe(const char* name, FnPtr fn);
void signalRemoveSubscription(const char* name, FnPtr fn);
void signalEmit(const char* name, void* userData);
void signalCleanUp(void);
}  // namespace utils

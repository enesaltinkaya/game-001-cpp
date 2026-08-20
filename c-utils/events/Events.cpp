#include "Events.h"

// --- String-based signals ---

namespace utils {
static std::unordered_map<std::string, std::vector<FnPtr>> signals;

void signalSubscribe(const char* name, FnPtr fn) {
    auto& list = signals[name];
    for (FnPtr existing : list) {
        if (existing == fn) {
            return;
        }
    }
    list.push_back(fn);
}

void signalRemoveSubscription(const char* name, FnPtr fn) {
    auto it = signals.find(name);
    if (it == signals.end()) return;
    auto& list = it->second;
    for (i32 i = 0, s = static_cast<i32>(list.size()); i < s; i++) {
        if (list[i] == fn) {
            list[i] = list.back();
            list.pop_back();
            return;
        }
    }
}

void signalEmit(const char* name, void* userData) {
    auto it = signals.find(name);
    if (it == signals.end()) return;
    for (FnPtr fn : it->second) {
        fn(userData);
    }
}

void signalCleanUp(void) {
    signals.clear();
}}  // namespace utils
